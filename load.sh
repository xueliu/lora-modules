#!/bin/sh

rmmod lora-sx1301
rmmod lora-sx1257
rmmod lora-rn2483
rmmod lora-wimod
rmmod lora-usi
rmmod lora-rak811
rmmod lora-dev
rmmod lora

set -e

BDIR=linux

insmod ${BDIR}/net/lora/lora.ko

insmod ${BDIR}/drivers/net/lora/lora-dev.ko
insmod ${BDIR}/drivers/net/lora/lora-rn2483.ko dyndbg
insmod ${BDIR}/drivers/net/lora/lora-wimod.ko dyndbg
insmod ${BDIR}/drivers/net/lora/lora-usi.ko dyndbg
insmod ${BDIR}/drivers/net/lora/lora-rak811.ko dyndbg
insmod ${BDIR}/drivers/net/lora/lora-sx1257.ko dyndbg
insmod ${BDIR}/drivers/net/lora/lora-sx1301.ko dyndbg