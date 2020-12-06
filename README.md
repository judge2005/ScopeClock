# ScopeClock

An ESP32 oscilloscope clock that uses the OSC7.0 hardware available here: https://oscilloscopeclocknixiecrt.com/Kit.htm. All of the functions that actually draw the clock faces are the copyright of and were provided by Grahame Marsh of http://www.sgitheach.org.uk/

Note that if you use this software, I recommend you get a new ESP32 board and program that, so you always have the original
application to fall back on. Also note that you will need to provide a separate 5V power supply for the ESP32 - I also removed the
diode that leads to the ESP32 5V pin so that there is no possibility of it leaking back in to the scope hardware 5V rail.
