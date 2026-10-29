// Gas Monitor website settings.
// Edit, commit and push to GitHub: the site updates about a minute later. No re-flash needed.
window.GAS_CONFIG = {
  // Supabase dashboard -> Project Settings -> API (or "Connect" button).
  // The anon / publishable key is public by design: with it, people can only READ readings.
  // Never put the service_role / secret key here.
  supabaseUrl: 'https://ijwyxyxrwtrxmbdkhmcn.supabase.co',
  supabaseKey: 'sb_publishable_GVjJAPieCJVGzBpyFLMztw_4OWz847w',  // anon key (starts with eyJ...) or publishable key (sb_publishable_...)

  // tank: the number the ESP32 sends (1 = first entry in TANKS[] in the sketch).
  // fullMm:  liquid height the sensor shows right after a refill   <- CALIBRATE
  // emptyMm: liquid height when the cylinder runs out              <- CALIBRATE
  // fullTempC: sensor temperature when fullMm was read (levels are corrected to this temperature)
  // Changing these recalculates all history too, because the database stores millimetres.
  tanks: [
    {tank: 1, name: 'Tank 1', mac: 'f1:81:73:69:2c:5b', capacityKg: 48, fullMm: 836, fullTempC: 26, emptyMm: 20, diameterMm: 365},
    {tank: 2, name: 'Tank 2', mac: 'e9:e1:cf:08:46:7c', capacityKg: 48, fullMm: 836, fullTempC: 26, emptyMm: 20, diameterMm: 365},
  ],

  // Liquid LPG grows about 0.27% in volume per °C (typical propane/butane mix).
  // Set to 0 to turn the temperature correction off.
  expansionPerC: 0.0027,

  hubOfflineMin: 20,        // no upload for this long -> "Hub offline" warning
};
