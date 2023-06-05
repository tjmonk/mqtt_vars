# mqttvars
VarServer MQTT Interface

## Overview

The mqttvars service maps VarServer variables to MQTT topics on a
remote MQTT broker.  The VarServer variable mapping is specified
via a JSON configuration file.  Each VarServer variable is mapped
to its own topic and is specified to be Publish and/or Subscribe.
The topic QoS ( Quality of Service ) is also specified.

In general it is not recommended to associate a single variable
with the same topic in both publish and subscribe as this creates
a message bouncing effect resulting in inefficient use of bandwidth,
since the MQTT broker sends a subscription variable change notification
back to the client when the client publishes.

## Configuration File

The configuration file has a single root level attribute "mqttvars" which
contains a list of mqtt variable/topic mappings.  Each variable/topic mapping
contains the following attributes:

- var : the name of the VarServer variable
- qos : the quality of service - 0, 1, or 2
- mapping: p (publish), s (subscribe), or ps (publish and subscribe)

## Example Configuration File

```
{ "mqttvars":[
  { "var":"/sys/test/a", "qos":1, "mapping" : "p" },
  { "var":"/sys/test/b", "qos":1, "mapping" : "p" },
  { "var":"/sys/test/f", "qos":0, "mapping" : "p" },
  { "var":"/sys/test/c", "qos":1, "mapping" : "s" }
  ]
}

```

## Command Line Arguments

The behavior of the mqttvars service can be modified via its command line
arguments.  The following command line arguments are supported:

| | | |
|---|---|---|
| Arg | Description | Default Value |
| -v | Set verbose output | |
| -h | Display help |  |
| -f | Specify Configuration File |  |
| -a | MQTT Broker Address | tcp://test.mosquitto.org:1883 |
| -i | MQTT Client Identifier | MQTTVarsClient |
| -u | MQTT Username | |
| -p | MQTT Password | |

The only mandatory argument is the configuration file specifier.

## Prerequisites

The mqttvars service requires the following components:

- varserver : variable server ( https://github.com/tjmonk/varserver )
- tjson : JSON parser library ( https://github.com/tjmonk/libtjson )
- paho_mqtt3a : Paho MQTT3 Async library ( https://github.com/eclipse/paho.mqtt.c )

## Build

```
./build.sh
```

## Run the Example

```
varserver &

mkvar -t uint16 -n /sys/test/a
mkvar -t uint32 -n /sys/test/b
mkvar -t float -n /sys/test/f
mkvar -t str -n /sys/test/c

mqttvars -f test/mqtt.json &
```

Note that you will need an external utility to connect to the server and
validate the operation of the mqttvars service.

Changes to /sys/test/a, sys/test/b, and /sys/test/f on the client should be
replicated to the broker.  Changes to /sys/test/c on the broker should be
replicated to the client.

## Example output

```
mqttvars -v -f test/mqtt.json &
[2] 91018
creating client
connecting to: tcp://test.mosquitto.org:1883 as (null)
root@40eccdc29332:~/tgp/mqtt_vars# Registered for notifications on /sys/test/a
Registered for notifications on /sys/test/b
Registered for notifications on /sys/test/f
Successful connection
/sys/test/c:0
Subscription successful

root@40eccdc29332:~/tgp/mqtt_vars# setvar /sys/test/a 12
OK
Send message to /sys/test/a
Message with token value 0 delivery confirmed
root@40eccdc29332:~/tgp/mqtt_vars# setvar /sys/test/b 20
OK
Send message to /sys/test/b
Message with token value 0 delivery confirmed
root@40eccdc29332:~/tgp/mqtt_vars# setvar /sys/test/f 5.4
OK
Send message to /sys/test/f
Message with token value 0 delivery confirmed
root@40eccdc29332:~/tgp/mqtt_vars# Message arrived
    topic: /sys/test/c
    Received message: /sys/test/c
    value: Hello World
Hello World
getvar /sys/test/c
Hello World

```