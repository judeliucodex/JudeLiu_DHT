# Self-Reflection — ESP32 + DHT22 Temperature & Humidity Logger

This project was fun in a way I did not expect from a school assignment. The goal was simple — read
a temperature and humidity sensor and show the numbers somewhere useful — but every layer of it had
something new to teach, and I never got tired of watching a reading appear on the OLED and then on
the website a few seconds later.

The software, honestly, was fairly easy. Once the libraries were installed, the code was mostly a
matter of reading the datasheet and being careful: the DHT22 needs at least two seconds between
reads, GPIO 4 is safe to use while GPIO 0, 2, 5, 12 and 15 change the boot mode, and both modules
are 3.3 V parts so nothing needed level shifting. Debugging usually meant changing one thing at a
time. Compared with the hardware side, writing the firmware felt comfortable.

Drawing the PCB was a different story, and it was humbling. I assumed my devkit had 38 pins in two
rows of 19, so I designed the whole socket around that — then measured the real board and found 30
pins in two rows of 15, 22.86 mm apart. The socket could not physically fit. I had to rebuild the
footprint from two 1×15 headers, redo the placement, and re-route all the copper, ground planes
included. It took far longer than I expected.

But I learned more from that mistake than from anything that went right. I now understand pin pitch
and courtyards, why the 3V3 track is sized by the current it carries rather than by what looks
tidy, and why silkscreen orientation marks matter. If I built this again, I would measure every part
first — checking assumptions against the physical object before designing around them is the lesson
I am keeping.
