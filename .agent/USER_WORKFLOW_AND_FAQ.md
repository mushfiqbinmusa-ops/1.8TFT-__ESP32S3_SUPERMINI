# PhotoFrame Usage Guide & Answers to Your Questions

This document answers all your questions about how the photo frame operates, using your Android phone, whether the Raspberry Pi needs to stay on, Docker automatic startup, and alternative ways to use the display.

---

## Table of Contents
1. [How Do I Change the Image Using My Android Phone?](#1-how-do-i-change-the-image-using-my-android-phone)
2. [Is It Mandatory to Keep the Raspberry Pi Running All the Time?](#2-is-it-mandatory-to-keep-the-raspberry-pi-running-all-the-time)
3. [TFT Display vs. E-Paper Display (Important Difference)](#3-tft-display-vs-e-paper-display-important-difference)
4. [Does Docker Turn On Automatically When the Raspberry Pi Starts?](#4-does-docker-turn-on-automatically-when-the-raspberry-pi-starts)
5. [Summary of Two Possible Ways to Run Your Project](#5-summary-of-two-possible-ways-to-run-your-project)

---

## 1. How Do I Change the Image Using My Android Phone?

### Can I just open a web address on my phone and upload a photo?
**Yes, absolutely!** The web dashboard built into this repository is a modern, responsive web application (Vue 3) designed to work on mobile phone browsers as well as desktop computers.

### Step-by-step workflow from your Android phone:
1. Make sure your Android phone is connected to the **same Wi-Fi network** as your Raspberry Pi.
2. Open Google Chrome (or any browser) on your phone.
3. Enter the Raspberry Pi's IP address and port:
   ```text
   http://192.168.1.XXX:9607
   ```
   *(or `http://raspberrypi.local:9607` if your router supports mDNS).*
4. Log in with your admin username and password.
5. Tap the **Gallery** tab:
   - Tap **"Upload"** or **"Add Photos"**.
   - Select any picture directly from your phone's photo gallery or camera.
   - To remove an old photo, tap the photo and tap the **Delete** (trash) icon.
6. **What happens automatically**:
   - The server instantly receives the photo.
   - It automatically crops and resizes it to your display's resolution (**160 × 128** or **128 × 160**).
   - If you turned on the clock, date, or weather overlays in the device settings, the server adds them automatically.
7. **How the ESP32 updates**:
   - The ESP32 wakes up periodically (for example, every 5 or 15 minutes) or whenever rebooted, checks the server, downloads the newly processed picture, and displays it on the 1.8" screen!

---

## 2. Is It Mandatory to Keep the Raspberry Pi Running All the Time?

### Direct Answer: **NO, it is NOT mandatory!**

You do **not** need to leave your Raspberry Pi powered on 24 hours a day, 7 days a week.

### How it works if the Raspberry Pi is turned OFF:
1. **The ESP32 stores the picture in its internal Flash Memory (`LittleFS`)**:
   - When the ESP32 downloads the image from the Raspberry Pi, it saves the file directly into its onboard 4MB flash storage.
   - Once saved to flash, the ESP32 can reboot or run indefinitely without the Raspberry Pi.
2. **If the Raspberry Pi is OFF**:
   - The ESP32 simply keeps displaying the saved picture.
   - Even if you unplug the ESP32 and plug it back in, it boots up, sees the Raspberry Pi is offline, and immediately loads the last saved photo from its own internal flash storage onto the screen!
3. **When you want to change the picture**:
   - Power on the Raspberry Pi.
   - Docker starts automatically (see Section 4).
   - Open your phone browser, upload the new picture to `http://<PI_IP>:9607`.
   - The ESP32 downloads the new picture and writes it to flash.
   - **You can now shut down or unplug the Raspberry Pi!** The new image will stay on your frame.

---

## 3. TFT Display vs. E-Paper Display (Important Difference)

Because you are using a **1.8-inch TFT LCD (ST7735)** rather than an **E-Paper (E-Ink) display**, here is an important technical detail to keep in mind:

| Feature | E-Paper (E-Ink) Display | 1.8" TFT Color Display (ST7735) |
|:---|:---|:---|
| **Display when power is unplugged** | Stays visible forever with 0 electricity. | Screen goes black if power to the ESP32 is unplugged. |
| **Colors & Refresh Speed** | Slow refresh (10–30s), limited colors. | Instant refresh (<1s), 65,536 vivid colors. |
| **Power Consumption** | Ultra-low (sleeps 99% of time). | Needs 5V power to the ESP32 (via phone charger or power bank). |
| **Does it need Raspberry Pi to stay on?** | No. | **No.** As long as the ESP32 has USB power, it keeps showing the picture. |

**Key Takeaway**: 
- Your **ESP32-S3** needs to stay plugged into a standard 5V USB charger (or power bank) to keep the screen backlight lit.
- Your **Raspberry Pi** does **NOT** need to stay plugged in or running.

---

## 4. Does Docker Turn On Automatically When the Raspberry Pi Starts?

### Direct Answer: **YES, completely automatically!**

You do **not** have to open a terminal or run manual commands every time you turn on your Raspberry Pi.

### How to ensure automatic startup:
In Docker, containers have a **restart policy**. When configured with `restart: unless-stopped` or `restart: always`, the Linux system registers the container with `systemd`.

Whenever your Raspberry Pi powers on or reboots:
1. The Linux operating system boots.
2. The Docker daemon starts automatically in the background.
3. Docker checks its registered containers and **automatically starts `photoframe-server`**.
4. Within 15–30 seconds of plugging in your Pi, the web dashboard is live and ready at `http://<PI_IP>:9607`.

### Verification (Done once on the Raspberry Pi):
To make sure Docker itself is enabled to boot with Linux:
```bash
sudo systemctl enable docker
```

And your `docker-compose.yml` file already has:
```yaml
restart: unless-stopped
```
This guarantees zero manual maintenance. Turn the Pi on -> it works. Turn the Pi off -> it stops safely.

---

## 5. Summary of Two Possible Ways to Run Your Project

You have two practical architectural choices:

### Architecture 1: Raspberry Pi Server + ESP32 Frame (This Repository)
```
[ Android Phone ] --(Uploads Photo via Web)---> [ Raspberry Pi Server (Docker) ]
                                                            |
                                                (Auto-crops to 160x128 & adds overlay)
                                                            |
                                                            v
                                                [ ESP32-S3 SuperMini ]
                                                            |
                                                    (Saves to Flash)
                                                            |
                                                            v
                                                [ 1.8" TFT ST7735 ]
```
- **Pros**:
  - Automatically handles picture resizing, cropping, and orientation.
  - Can pull automatically from **Google Photos**, **Immich**, **Synology**, **Unsplash**, or **Pexels**.
  - Adds real-time **Weather**, **Date**, **Time**, or **Calendar** overlays on top of your photos.
  - Pi only needs to be on when uploading/rotating photos; can be turned off afterwards.
- **Cons**: Requires having a Raspberry Pi or home server running Docker.

---

### Architecture 2: Standalone ESP32 (No Raspberry Pi at All!)
```
[ Android Phone ] --(Uploads photo directly over Wi-Fi)---> [ ESP32-S3 SuperMini ]
                                                                     |
                                                             (Saves to Flash)
                                                                     |
                                                                     v
                                                             [ 1.8" TFT ST7735 ]
```
- **How it works**:
  - The ESP32-S3 runs its own mini Wi-Fi Web Server.
  - You open your phone browser, go to `http://192.168.1.XXX` (the ESP32's IP), select a picture from your phone, and hit "Upload".
  - The ESP32 receives the file, saves it to its internal flash memory, and displays it on the screen immediately.
- **Pros**: 100% standalone! **No Raspberry Pi, No Docker, No server needed.**
- **Cons**: Cannot automatically fetch from cloud services (Google Photos/Unsplash) or add live weather/calendar overlays.

---

> [!TIP]
> The second file created in this folder, [AI_AGENT_CODING_INSTRUCTIONS.md](file:///root/esp32-photoframe-server/AI_AGENT_CODING_INSTRUCTIONS.md), gives an AI coding agent complete instructions and code templates to implement **both** modes so you can choose whichever setup you like best!
