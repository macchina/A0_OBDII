#!/bin/bash
~/.local/bin/aws s3 cp ESP32_OBDII.ino.esp32.bin s3://media3.evtv.me
~/.local/bin/aws s3api put-object-acl --bucket media3.evtv.me --key ESP32_OBDII.ino.esp32.bin --acl "public-read"

