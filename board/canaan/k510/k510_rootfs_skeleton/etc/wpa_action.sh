#!/bin/sh
# Called by "wpa_cli -a" on wpa_supplicant events: DHCP client for station mode.
# In setup-AP mode (/var/run/<iface>.ap exists) the parking daemon owns the address.
IFACE=$1
EVENT=$2
PIDFILE=/var/run/udhcpc.$IFACE.pid

[ -f /var/run/$IFACE.ap ] && exit 0

case "$EVENT" in
CONNECTED)
	[ -f $PIDFILE ] && kill $(cat $PIDFILE) 2>/dev/null
	udhcpc -i $IFACE -b -p $PIDFILE -s /usr/share/udhcpc/default.script
	;;
DISCONNECTED)
	[ -f $PIDFILE ] && kill $(cat $PIDFILE) 2>/dev/null
	rm -f $PIDFILE
	ifconfig $IFACE 0.0.0.0
	;;
esac
exit 0
