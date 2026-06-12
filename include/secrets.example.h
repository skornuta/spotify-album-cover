#pragma once
//
// Copy this file to "secrets.h" (same folder) and fill in your own values.
// secrets.h is git-ignored so your credentials never get committed.
//

// ---- Wi-Fi ----
#define WIFI_SSID "your-wifi-ssid"
#define WIFI_PASS "your-wifi-password"

// ---- Spotify app credentials (from https://developer.spotify.com/dashboard) ----
#define SPOTIFY_CLIENT_ID     "your-spotify-client-id"
#define SPOTIFY_CLIENT_SECRET "your-spotify-client-secret"

// ---- Long-lived OAuth2 refresh token (see README "Getting a refresh token") ----
#define SPOTIFY_REFRESH_TOKEN "your-spotify-refresh-token"

// ---- Optional ----
// #define SPOTIFY_MARKET "US"
//
// To validate TLS certificates instead of using setInsecure(), paste the
// Spotify/CDN root CA in PEM form and the firmware will pin it automatically:
// #define SPOTIFY_ROOT_CA R"EOF(
// -----BEGIN CERTIFICATE-----
// ...
// -----END CERTIFICATE-----
// )EOF"
