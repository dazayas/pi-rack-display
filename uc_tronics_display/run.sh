#!/usr/bin/with-contenv bashio

echo "Start!"

if ls /dev/i2c-1; then 
    echo "Found i2c access!";
    echo "Loading C script for UCTRONICS OLED...";

    # Passed to the display binary as environment. ip_address is optional --
    # unset means detect -- so it is only exported when actually configured.
    if bashio::config.has_value 'ip_address'; then
        export UCTRONICS_IP_ADDRESS="$(bashio::config 'ip_address')"
        echo "Using configured IP: ${UCTRONICS_IP_ADDRESS}"
    fi
    export UCTRONICS_DWELL_SECONDS="$(bashio::config 'dwell_seconds')"
    echo "Screen dwell: ${UCTRONICS_DWELL_SECONDS}s"

    cd /lcd_display/
    # make clean
    make 

    echo "UCTRONICS OLED Display now be showing information";
    ./display
else
echo "no found i2c!"
fi