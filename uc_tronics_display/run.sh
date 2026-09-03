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
    export UCTRONICS_CYCLE_SECONDS="$(bashio::config 'cycle_seconds')"
    export UCTRONICS_ALIGN_SECONDS="$(bashio::config 'align_seconds')"
    export UCTRONICS_ALIGN_OFFSET_SECONDS="$(bashio::config 'align_offset_seconds')"

    # The container has no systemd, so the display cannot ask timedatectl
    # whether the clock is synchronised; it says so once and aligns anyway.
    # Home Assistant OS keeps the host's clock itself.
    if [ "${UCTRONICS_ALIGN_SECONDS}" = "0" ]; then
        echo "Free-running, screen dwell: ${UCTRONICS_DWELL_SECONDS}s"
    else
        echo "Cycle: ${UCTRONICS_CYCLE_SECONDS}s, aligned to every ${UCTRONICS_ALIGN_SECONDS}s of the wall clock, offset ${UCTRONICS_ALIGN_OFFSET_SECONDS}s"
    fi

    cd /lcd_display/
    # make clean
    make 

    echo "UCTRONICS OLED Display now be showing information";
    ./display
else
echo "no found i2c!"
fi