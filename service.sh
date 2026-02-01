#!/system/bin/sh
MODDIR=${0%/*}

# Wait for boot to complete somewhat
sleep 20

# Bootloop Protection
BOOT_COUNT_FILE="$MODDIR/boot_count"
MAX_RETRIES=3

# Check if module is explicitly disabled
if [ -f "$MODDIR/disable" ]; then
    echo "Module disabled by user or bootloop protection." >> "$MODDIR/boot_log.txt"
    exit 0
fi

current_count=$(cat "$BOOT_COUNT_FILE" 2>/dev/null || echo 0)
new_count=$((current_count + 1))

echo "$new_count" > "$BOOT_COUNT_FILE"

if [ "$new_count" -ge "$MAX_RETRIES" ]; then
    # Too many boots without successful completion
    touch "$MODDIR/disable"
    echo "Bootloop detected! Module disabled." >> "$MODDIR/boot_log.txt"
    exit 1
fi

# Reset counter after successful boot (60s delay)
(
    sleep 60
    echo 0 > "$BOOT_COUNT_FILE"
) &

# --- VCam Bind Mount Logic ---
TARGET_HAL="/vendor/lib64/hw/camera.qcom.so"
WRAPPER_LIB="$MODDIR/system/lib64/hw/libvcam_wrapper.so"
BACKUP_DIR="/dev/vcam"
BACKUP_HAL="$BACKUP_DIR/camera.qcom.orig.so"

log_print() {
    echo "$1" >> "$MODDIR/boot_log.txt"
    log -t VCamService "$1"
}

if [ -f "$TARGET_HAL" ]; then
    log_print "Target HAL found at $TARGET_HAL"
    
    # 1. Prepare Backup Location (tmpfs)
    mkdir -p "$BACKUP_DIR"
    chmod 755 "$BACKUP_DIR"
    # Ensure context allows access
    chcon u:object_r:device:s0 "$BACKUP_DIR"

    # 2. Copy Original HAL
    # We copy every boot because /dev is volatile
    cp "$TARGET_HAL" "$BACKUP_HAL"
    chmod 644 "$BACKUP_HAL"
    # CRITICAL: Set context so generic HAL domain can read it
    # 'vendor_file' is usually readable by hal_camera_default
    chcon u:object_r:vendor_file:s0 "$BACKUP_HAL"
    
    # 3. Bind Mount Wrapper
    if [ -f "$WRAPPER_LIB" ]; then
        log_print "Mounting wrapper..."
        mount -o bind "$WRAPPER_LIB" "$TARGET_HAL"
        if [ $? -eq 0 ]; then
            log_print "Mount success. Restarting camera provider..."
            # Kill provider to force reload
            killall android.hardware.camera.provider@2.4-service_64 || true
            killall cameraserver || true
        else
            log_print "Mount failed!"
        fi
    else
        log_print "Wrapper lib not found at $WRAPPER_LIB"
    fi
else
    log_print "Original HAL not found!"
fi

# 1. SELinux Patching
# We use sepolicy.rule for cleaner updates, but keep a minimal fallback if needed
# or just rely on the rule injection.
# Ensure executable permissions
chmod +x $MODDIR/cameraserver_proxy

# Start the daemon
nohup $MODDIR/system/bin/cameraserver_proxy > /dev/null 2>&1 &daemon
# Naming it 'cameraserver_proxy' to blend in
nohup $MODDIR/cameraserver_proxy > /data/local/tmp/vcam_receiver.log 2>&1 &

# Notify User
cmd notification post -S bigtext -t "VCam Status" "Service Started" "VCam"
