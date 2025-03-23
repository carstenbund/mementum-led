# esp32-matrix-group
Group of Matrix share text to display

This project aims to build a networked swarm of wearable devices, displaying syncronized messages. 

Implemented with Arduino IDE, for ESP32-S3 with integrated display

This codebase includes both client and server, defined by config choice at runtime.

Maximum devices are about 20 in this configuration.

The system boots, evaluated configuration to set mode and network specifics.
When configures as server the system will create a softAP to allow clients to connct and register.

Clients search for the configured network and register with the server, to signal readines for display.

The server exposes an admin interface to allow an operator to send text for display in to a queue, which is the broadcasted in parallel using threads to the redisteted clients.

Todo:
automated text, time based text, repetitions,
dedicated server to increase client count.
Use of multicast protocol.

