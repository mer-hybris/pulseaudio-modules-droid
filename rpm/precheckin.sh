#!/bin/sh

DEVICES="boston mako sbj"

for dev in $DEVICES; do
  sed -e "s/@DEVICE@/$dev/g" pulseaudio-modules-droid.spec.in > pulseaudio-modules-droid-$dev.spec
done
