# 🛰️ Image Classification Unit (ICU)

![Project Status](https://img.shields.io/badge/Status-Under%20Development-yellow)

Onboard CNN Image Classification for CubeSats, running on a low-power SBC. By processing data where it's collected, it saves precious downlink bandwidth and enables near-real-time decision-making in orbit.

---

## 🚀 Overview

CubeSat missions are bottlenecked by downlink bandwidth. The **Image Classification Unit (ICU)** tackles this by performing inference directly on the satellite. Using a Convolutional Neural Network (CNN) on a Raspberry Pi Zero 2W, it identifies and prioritizes valuable imagery (e.g., wildfires, ships, clouds), ensuring that only the most critical data is transmitted to Earth.

---

## 🧩 Features

- **Near-Real-Time Inference:** Embedded CNN using TensorFlow Lite for onboard image classification.
- **Efficient Memory Management:** NOR Flash storage with tombstone-based garbage collection.
- **Modular Architecture:** Clear separation between the SBC (RPi Zero 2W) and the mission manager (PIC18F67J94) via UART.
- **Extensible Interface:** Configurable image selection criteria and robust testing/debugging tools.

---

## ⚙️ Repository Structure

```
icu/
├── mcu/           — Embedded C firmware for PIC18F67J94 (CCS C Compiler)
├── sbc/           — Python application for Raspberry Pi (capture + inference)
└── docs_site/     — Public documentation site (GitHub Pages)
```

---

##  Hardware & Software

**Hardware**
- Raspberry Pi Zero 2W
- PIC18F67J94 Microcontroller
- MT25QL01GBBB 128MB Serial NOR Flash
- 12 MP Camera Module (16mm lens)

**Software**
- Python 3.11+
- TensorFlow Lite, OpenCV
- CCS C Compiler (v5.101)

---

## Build

**MCU Firmware**
```bash
cd mcu
make clean && make
```

**SBC Application**
```bash
cd sbc
python3 -m venv .virtual && source .virtual/bin/activate
pip install -r requirements.txt
python3 main.py
```

**Documentation (Doxygen)**
```bash
cd mcu
doxygen Doxyfile
# Output: docs/html/
```

**Documentation (Sphinx)**
```bash
cd sbc
python3 -m venv .virtual && source .virtual/bin/activate
pip install -r docs/requirements.txt
make -C docs html
# Output: docs/build/html/
```

---

## 🤝 Contributing

This project is developed in collaboration with **Agencia Espacial del Paraguay** & **Grupo de Investigacion en Electronica y Mecatronica**. Contributions and suggestions are welcome — open an Issue or submit a Pull Request.

---

## 📄 License

This project is licensed under the MIT License — see the LICENSE file for details.
