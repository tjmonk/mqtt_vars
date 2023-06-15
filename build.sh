#!/bin/sh

BASEDIR=`pwd`

# get and build paho-mqtt
git clone https://github.com/eclipse/paho.mqtt.c.git
cd paho.mqtt.c
make
sudo make install

# Build mqttvars
cd $BASEDIR
mkdir -p build && cd build
cmake ..
make
sudo make install
cd ..
