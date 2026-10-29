-- Gas Monitor database.
-- Paste this whole file into Supabase -> SQL Editor -> New query, and click Run.
-- Safe to run again: tables are only created if missing; functions and the view are replaced.
--
-- Security model
--   * The website and the ESP32 both use the public anon/publishable key.
--   * Anyone with that key can READ readings, but nobody can write to the table directly.
--   * The only way in is public.ingest(), which needs the secret device key from secrets.h.
--     Only the SHA-256 hash of that key is stored here.

create extension if not exists pgcrypto with schema extensions;

-- ---------------------------------------------------------------- readings
create table if not exists public.readings (
  id         bigint generated always as identity primary key,
  ts         timestamptz not null default now(),   -- end of the 5-minute window
  tank       smallint    not null check (tank between 1 and 4),
  level_mm   real,          -- median of quality>=2 reads in the window; null if there were none
  quality    smallint,      -- last quality seen (0..3); null if the sensor wasn't heard
  temp_c     smallint,
  battery_v  real,
  rssi       smallint,
  samples    smallint,      -- how many good reads went into level_mm
  unique (tank, ts)         -- a re-sent row (lost reply, retry) is ignored instead of duplicated
);
-- fast "last good level" lookup per tank
create index if not exists readings_good_level on public.readings (tank, ts desc) where level_mm is not null;

alter table public.readings enable row level security;
drop policy if exists "anyone can read" on public.readings;
create policy "anyone can read" on public.readings for select to anon, authenticated using (true);
revoke all on public.readings from anon, authenticated;
grant select on public.readings to anon, authenticated;

-- ---------------------------------------------------------------- device keys
create table if not exists public.device_keys (
  key_hash   text primary key,           -- sha256 hex of the device key
  label      text,
  created_at timestamptz not null default now()
);
alter table public.device_keys enable row level security;   -- no policies: invisible through the API
revoke all on public.device_keys from anon, authenticated;

-- ---------------------------------------------------------------- write path (ESP32)
-- POST /rest/v1/rpc/ingest  {"p_key": "<device key>", "p_rows": [{ts, tank, level_mm, ...}, ...]}
-- Returns how many rows were new.
create or replace function public.ingest(p_key text, p_rows jsonb)
returns int
language plpgsql security definer set search_path = '' as $$
declare n int;
begin
  if p_key is null or not exists (
       select 1 from public.device_keys
       where key_hash = encode(extensions.digest(p_key, 'sha256'), 'hex')) then
    raise exception 'unauthorized' using errcode = '28000';
  end if;
  if jsonb_typeof(p_rows) is distinct from 'array' or jsonb_array_length(p_rows) > 600 then
    raise exception 'p_rows must be a JSON array of at most 600 rows' using errcode = '22023';
  end if;

  insert into public.readings (ts, tank, level_mm, quality, temp_c, battery_v, rssi, samples)
  select coalesce((r->>'ts')::timestamptz, now()),
         (r->>'tank')::smallint,
         (r->>'level_mm')::real,
         (r->>'quality')::smallint,
         (r->>'temp_c')::smallint,
         (r->>'battery_v')::real,
         (r->>'rssi')::smallint,
         (r->>'samples')::smallint
  from jsonb_array_elements(p_rows) r
  on conflict (tank, ts) do nothing;
  get diagnostics n = row_count;
  return n;
end $$;
revoke all on function public.ingest(text, jsonb) from public, anon, authenticated;
grant execute on function public.ingest(text, jsonb) to anon;

-- ---------------------------------------------------------------- read path (website)
-- Bucketed history for the charts: median level and temperature per tank per bucket.
-- The website uses temp_c to correct the level for the liquid's thermal expansion.
-- POST /rest/v1/rpc/history  {"p_hours": 168, "p_bucket_min": 30}
-- Buckets are labelled by their start time.
drop function if exists public.history(int, int);   -- return columns changed over time; replace needs a drop
create function public.history(p_hours int, p_bucket_min int)
returns table (tank smallint, bucket timestamptz, level_mm real, temp_c real)
language sql stable set search_path = '' as $$
  select r.tank,
         date_bin(make_interval(mins => greatest(p_bucket_min, 1)), r.ts, '2000-01-01'::timestamptz),
         (percentile_cont(0.5) within group (order by r.level_mm))::real,
         (percentile_cont(0.5) within group (order by r.temp_c))::real
  from public.readings r
  where r.ts > now() - make_interval(hours => least(greatest(p_hours, 1), 24 * 400))
    and r.level_mm is not null
  group by 1, 2
  order by 1, 2;
$$;
revoke all on function public.history(int, int) from public, anon, authenticated;
grant execute on function public.history(int, int) to anon, authenticated;

-- Newest row per tank (shows whether the hub and sensor are alive), plus the newest
-- row that had a good level, since the newest row may have level_mm = null.
-- GET /rest/v1/latest?select=*
drop view if exists public.latest;
create view public.latest with (security_invoker = true) as
  select r.*, g.level_mm as good_level_mm, g.ts as good_ts, g.temp_c as good_temp_c
  from generate_series(1, 4) as t(tank)
  cross join lateral (
    select * from public.readings x where x.tank = t.tank order by x.ts desc limit 1) r
  left join lateral (
    select y.level_mm, y.ts, y.temp_c from public.readings y
    where y.tank = t.tank and y.level_mm is not null order by y.ts desc limit 1) g on true;
revoke all on public.latest from anon, authenticated;
grant select on public.latest to anon, authenticated;

-- ---------------------------------------------------------------- next step
-- Register the board's key: run supabase/add_device_key.local.sql (made for you, git-ignored).
-- Data volume is about 25 MB a year, so no clean-up job is needed for a long time.
