#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 turtle2a <354865938+turtle2a@users.noreply.github.com>


#note ix and other have more

#EG  TSO4,TSO6,LRO,WOL_UCAST,WOL_MCAST,WOL_MAGIC,VLAN_HWFILTER,VLAN_HWTSO,RXCSUM_IPV6,TXCSUM_IPV6

# 211126

#LO

ifconfig lo0 -txcsum


#OCTEPS 

ifconfig octep0 -vlanhwcsum -wol_magic -wol_ucast -wol_mcast -tso -txcsum6 -txcsum -lro -tso4 -tso6 -vlanhwfilter -vlanhwtag -vlanmtu polling
ifconfig octep1 -vlanhwcsum -wol_magic -wol_ucast -wol_mcast -tso -txcsum6 -txcsum -lro -tso4 -tso6 -vlanhwfilter -vlanhwtag -vlanmtu polling
ifconfig octep2 -vlanhwcsum -wol_magic -wol_ucast -wol_mcast -tso -txcsum6 -txcsum -lro -tso4 -tso6 -vlanhwfilter -vlanhwtag -vlanmtu polling
ifconfig octep3 -vlanhwcsum -wol_magic -wol_ucast -wol_mcast -tso -txcsum6 -txcsum -lro -tso4 -tso6 -vlanhwfilter -vlanhwtag -vlanmtu polling
ifconfig octep4 -vlanhwcsum -wol_magic -wol_ucast -wol_mcast -tso -txcsum6 -txcsum -lro -tso4 -tso6 -vlanhwfilter -vlanhwtag -vlanmtu polling
ifconfig octep5 -vlanhwcsum -wol_magic -wol_ucast -wol_mcast -tso -txcsum6 -txcsum -lro -tso4 -tso6 -vlanhwfilter -vlanhwtag -vlanmtu polling
ifconfig octep6 -vlanhwcsum -wol_magic -wol_ucast -wol_mcast -tso -txcsum6 -txcsum -lro -tso4 -tso6 -vlanhwfilter -vlanhwtag -vlanmtu polling
ifconfig octep7 -vlanhwcsum -wol_magic -wol_ucast -wol_mcast -tso -txcsum6 -txcsum -lro -tso4 -tso6 -vlanhwfilter -vlanhwtag -vlanmtu polling
ifconfig octep8 -vlanhwcsum -wol_magic -wol_ucast -wol_mcast -tso -txcsum6 -txcsum -lro -tso4 -tso6 -vlanhwfilter -vlanhwtag -vlanmtu polling
ifconfig octep9 -vlanhwcsum -wol_magic -wol_ucast -wol_mcast -tso -txcsum6 -txcsum -lro -tso4 -tso6 -vlanhwfilter -vlanhwtag -vlanmtu polling
ifconfig octep10 -vlanhwcsum -wol_magic -wol_ucast -wol_mcast -tso -txcsum6 -txcsum -lro -tso4 -tso6 -vlanhwfilter -vlanhwtag -vlanmtu polling
ifconfig octep11 -vlanhwcsum -wol_magic -wol_ucast -wol_mcast -tso -txcsum6 -txcsum -lro -tso4 -tso6 -vlanhwfilter -vlanhwtag -vlanmtu polling


#ENP



#IX's 

ifconfig ix0 -vlanhwcsum -wol_magic -wol_ucast -wol_mcast -tso -txcsum6 -txcsum -lro -tso4 -tso6 -vlanhwfilter -vlanhwtag -vlanmtu
ifconfig ix1 -vlanhwcsum -wol_magic -wol_ucast -wol_mcast -tso -txcsum6 -txcsum -lro -tso4 -tso6 -vlanhwfilter -vlanhwtag -vlanmtu polling

ifconfig ix2 -vlanhwcsum -wol_magic -wol_ucast -wol_mcast -tso -txcsum6 -txcsum -lro -tso4 -tso6 -vlanhwfilter -vlanhwtag -vlanmtu
ifconfig ix3 -vlanhwcsum -wol_magic -wol_ucast -wol_mcast -tso -txcsum6 -txcsum -lro -tso4 -tso6 -vlanhwfilter -vlanhwtag -vlanmtu


#IGC's 


ifconfig igc0 -vlanhwcsum -wol_magic -wol_ucast -wol_mcast -tso -txcsum6 -txcsum -lro -tso4 -tso6 -vlanhwfilter -vlanhwtag -vlanmtu

ifconfig igc1 -vlanhwcsum -wol_magic -wol_ucast -wol_mcast -tso -txcsum6 -txcsum -lro -tso4 -tso6 -vlanhwfilter -vlanhwtag -vlanmtu

ifconfig igc2 -vlanhwcsum -wol_magic -wol_ucast -wol_mcast -tso -txcsum6 -txcsum -lro -tso4 -tso6 -vlanhwfilter -vlanhwtag -vlanmtu




#IGB"S

ifconfig igb0 -vlanhwcsum -wol_magic -wol_ucast -wol_mcast -tso -txcsum6 -txcsum -lro -tso4 -tso6 -vlanhwfilter -vlanhwtag -vlanmtu


ifconfig igb1 -vlanhwcsum -wol_magic -wol_ucast -wol_mcast -tso -txcsum6 -txcsum -lro -tso4 -tso6 -vlanhwfilter -vlanhwtag -vlanmtu

ifconfig igb2 -vlanhwcsum -wol_magic -wol_ucast -wol_mcast -tso -txcsum6 -txcsum -lro -tso4 -tso6 -vlanhwfilter -vlanhwtag -vlanmtu


#EM"S

ifconfig em0 -vlanhwcsum -wol_magic -wol_ucast -wol_mcast -tso -txcsum6 -txcsum -lro -tso4 -tso6 -vlanhwfilter -vlanhwtag -vlanmtu


ifconfig em1 -vlanhwcsum -wol_magic -wol_ucast -wol_mcast -tso -txcsum6 -txcsum -lro -tso4 -tso6 -vlanhwfilter -vlanhwtag -vlanmtu polling

ifconfig em2 -vlanhwcsum -wol_magic -wol_ucast -wol_mcast -tso -txcsum6 -txcsum -lro -tso4 -tso6 -vlanhwfilter -vlanhwtag -vlanmtu


exit
