#!/bin/bash
cd "${0%/*}"
aws s3 cp ./ESP32_OBDII.ino.esp32.bin s3://media3.evtv.me --acl "public-read"
exit
