# etherswitchcfg invocations for the Edge 520B, applied by
# /usr/local/etc/rc.syshook.d/early/10-switch when MODE=configure.
#
# One invocation per line. First token = etherswitch unit number (so the unit
# the kernel hints put on mdio0 is 0), the rest are literal etherswitchcfg
# arguments. '#' starts a comment.
#
# NOTHING HERE IS ENABLED. The CPU-port index and which mdio bus is which
# switch are still guesses (the device hints are in
# overlay/boot/loader.conf.d/e6000sw.conf, installed as
# /boot/loader.conf.d/vce520b-switch.conf). Confirm them from a bootverbose
# dmesg first:
#
#   dmesg | grep -E 'igb[0-9]|mdio[0-9]|e6000sw'
#   etherswitchcfg -f /dev/etherswitch0 -v info
#
# bootverbose e6000sw prints "CPU port at N", "fixed port at N" and
# "PHY at port N" for every port. That output is the ground truth.
#
# ---------------------------------------------------------------------------
# PORT mode (the e6000sw default) - flat bridge, no tags.
#
# In PORT mode the vlangroup unit IS the port number and the VID is forced to
# port+1. e6000sw_set_port_vlan() REJECTS es_member_ports != es_untagged_ports
# ("Tagged ports not supported."), so every member must be untagged: no 't'
# suffix is legal here.
#
# With the shipped hints (port4cpu, port5/6disabled) enabled ports are 0-4 and
# stock e6000sw_init_vlan() already gives each of them members = the other four.
# That is the flat bridge, so there is nothing to write. These lines are only
# needed if the enabled-port set turns out to be different:
#
# 0 vlangroup0 members 1,2,3,4
# 0 vlangroup1 members 0,2,3,4
# 0 vlangroup2 members 0,1,3,4
# 0 vlangroup3 members 0,1,2,4
# 0 vlangroup4 members 0,1,2,3
#
# ---------------------------------------------------------------------------
# DOT1Q mode - needed only if a switch port must carry tagged frames (an OPNsense
# VLAN on a LAN port). Switching modes re-programs every port, so do this
# deliberately, not by default.
#
# 0 config vlan_mode dot1q
# 0 vlangroup0 vlan 1 members 0,1,2,3,4t
# 0 port0 pvid 1
#
# ---------------------------------------------------------------------------
# Raw register escape hatch (88E6176 REG_GLOBAL), for cross-checking against the
# vendor Linux driver's register names:
#   0x1b GLOBAL1, 0x1c GLOBAL2 (SMI cmd), 0x1d GLOBAL3
#   M88_SW_PORT_REG(port,reg) = ((0x10+port)<<5)|reg
#
# 0 reg 0x1b
# 0 reg 0x0400
