# CamWeave Camera for Linux

An open-source **native GTK 4 + GStreamer + V4L2** camera app. It reuses an existing webcam and serves a direct MJPEG stream to any browser or CamWeave Viewer on a reachable private network. No Electron, browser engine, cloud relay, account, audio, or recording. Licensed under MIT.

## Build

On Ubuntu 24.04 or a similar Linux distribution:

```sh
sudo apt install meson ninja-build pkg-config libgtk-4-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev gstreamer1.0-plugins-base gstreamer1.0-plugins-good
meson setup build
meson compile -C build
./build/camweave-camera
```

The app lists accessible V4L2 capture devices such as `/dev/video0`. Choose a camera, enter this computer's reachable local IP or private VPN DNS name, and start. The viewing link has a fixed, editable access code (default UUID). The browser can change 720p/1080p output and 5/10/15 FPS; up to four viewers can connect simultaneously. Device-level ISO is not exposed, so the optional ISO control in CamWeave Viewer is unavailable for Linux cameras.

For another device on the same Wi-Fi, no network software is needed. For remote access, connect both devices through a private VPN such as Tailscale, ZeroTier, NetBird, or a self-hosted option. The address field changes only the generated link. The service listens on available interfaces; use your firewall and private network to limit access. Never forward TCP 8080 publicly. Anyone with the complete link can watch and control stream settings.

## Install

```sh
sudo meson install -C build
```

The executable installs to the selected prefix's `bin` directory and its self-contained viewing page to `share/camweave`. Run it as the desktop user with access to the V4L2 device. The access code and address are saved under the user's config directory. Headless/Raspberry Pi deployment is a later target; this first Linux app requires a graphical session.

## Validation

CI builds on Ubuntu 24.04 and runs a real GTK/GStreamer process under Xvfb with a synthetic camera source. It verifies link authorization, browser page, control API, status and MJPEG transport. Physical USB cameras, ARM hardware, VPN switching, distro packaging, performance and long-running stability still require device testing. The synthetic source is enabled only by test environment variables in CI.
