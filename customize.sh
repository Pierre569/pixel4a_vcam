SKIPUNZIP=1

# Print Installation Message
ui_print "*******************************"
ui_print " Pixel 4a Virtual Camera Module"
ui_print "*******************************"

# Verify Device (Optional but recommended)
PROP=$(grep_prop ro.product.device)
if [ "$PROP" != "sunfish" ]; then
  ui_print "! Warning: Device is $PROP, expected sunfish (Pixel 4a)."
  ui_print "! Proceeding anyway..."
else
  ui_print "- Device confirmed: Pixel 4a (sunfish)"
fi

# Extract files
ui_print "- Extracting module files..."
unzip -o "$ZIPFILE" 'module.prop' 'service.sh' 'cameraserver_proxy' 'system/*' -d $MODPATH >&2

# Directory for Vendor HAL overlay
# On newer Android with Magisk, we can target /vendor directly via $MODPATH/vendor if supported,
# or usually $MODPATH/system/vendor on older setups. 
# Magisk documentation suggests using /system/vendor structure for system-as-root consistency or just /vendor.
# We will use $MODPATH/vendor to map to /vendor.

ORIG_HAL="/vendor/lib64/hw/camera.qcom.so"
TARGET_DIR="$MODPATH/vendor/lib64/hw"
mkdir -p "$TARGET_DIR"

if [ -f "$ORIG_HAL" ]; then
  ui_print "- Found original HAL at $ORIG_HAL"
  ui_print "- Creating backup copy for dlopen..."
  
  # Copy the original HAL to our module directory as `camera.qcom.orig.so`
  # This makes it appear in /vendor/lib64/hw/camera.qcom.orig.so when mounted
  cp "$ORIG_HAL" "$TARGET_DIR/camera.qcom.orig.so"
  
  # Move our wrapper to `camera.qcom.so`
  # We assume our wrapper was built and put in system/vendor/lib64/hw/camera.qcom.so inside the zip
  # Depending on build output structure. 
  # For now, let's assume the zip has the wrapper at root/camera.qcom.so for simplicity of this script moving it.
  # Or standardized path.
  
  # NOTE: Users must place the compiled `camera.qcom.vcam.so` as `camera.qcom.so` in the zip root or structured lib path.
  # Let's check for it.
  
  if [ -f "$MODPATH/camera.qcom.vcam.so" ]; then
      mv "$MODPATH/camera.qcom.vcam.so" "$TARGET_DIR/camera.qcom.so"
      ui_print "- Installed Virtual Camera Wrapper"
  else
      ui_print "! ERROR: camera.qcom.vcam.so not found in zip!"
      abort
  fi
  
else
  ui_print "! ERROR: Original HAL not found at $ORIG_HAL"
  abort
fi

# Permissions
set_perm_recursive $MODPATH 0 0 0755 0644
set_perm $MODPATH/service.sh 0 0 0755
set_perm $MODPATH/cameraserver_proxy 0 0 0755

ui_print "- Installation Complete. Reboot."
