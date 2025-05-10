#ifndef CREDENTIALS_H
#define CREDENTIALS_H

// Credentials example! Please change them to your own.

// WiFi Access Point (AP) settings
const char* ap_ssid = "Ponto-Eletronico";
const char* ap_password = "test54487";

// Web Interface Admin credentials
const char* adminUser = "admin";     // Change this
const char* adminPass = "password";  // Change this

// Known WiFi networks for client mode (for NTP updates)
#define MAX_WIFI_NETWORKS 2
struct WiFiNetwork {
  const char* ssid;
  const char* password;
};

WiFiNetwork availableNetworks[MAX_WIFI_NETWORKS] = {
  {"AvailableWifiNetworkName", "passwordOfSaidWifi"},
  {"SecondaryWifi", "password2"}
  // Add more networks if needed, and update MAX_WIFI_NETWORKS
};

#endif //CREDENTIALS_H