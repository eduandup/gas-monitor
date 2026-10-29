// Copy this file to secrets.h (same folder) and fill it in. secrets.h is git-ignored: never commit it.
#pragma once

#define WIFI_SSID         "YourWiFiName"
#define WIFI_PASS         "YourWiFiPassword"

// Supabase dashboard -> Project Settings -> API. Leave SUPABASE_URL empty to run without cloud upload.
#define SUPABASE_URL      "https://xxxxxxxxxxxxxxxxxxxx.supabase.co"
#define SUPABASE_ANON_KEY "your-anon-or-publishable-key"

// Random secret that lets this board write readings. Only its SHA-256 hash is stored in Supabase.
// Generate one with:  python -c "import secrets; print(secrets.token_urlsafe(32))"
#define DEVICE_KEY        "replace-with-a-long-random-string"
