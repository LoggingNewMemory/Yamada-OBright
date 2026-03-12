#!/bin/bash

# ================= CONFIGURATION =================
# CHANGE THIS to your actual NDK path if not set in environment
NDK_ROOT=${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/28.2.13676358}

# Target Architecture and API Level
API_LEVEL=30  # Android 11 (Safe baseline for modern ROMs)
TARGET="aarch64-linux-android${API_LEVEL}"

# Output Names (Updated to match Yamada OBright Diagram)
BIN_NAME="yamada.obright-V1@3.0-service"
RC_NAME="init.yamada.obright.rc"

# Output Directories
OUT_DIR="out"
BIN_DIR="$OUT_DIR/vendor/bin/hw"
ETC_DIR="$OUT_DIR/vendor/etc/init"
# =================================================

# 1. Check NDK
if [ ! -d "$NDK_ROOT" ]; then
    echo "Error: NDK not found at $NDK_ROOT"
    echo "Please set ANDROID_NDK_HOME or edit the script."
    exit 1
fi

TOOLCHAIN="$NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin"
CC="$TOOLCHAIN/clang"

if [ ! -f "$CC" ]; then
    echo "Error: Clang not found at $CC"
    exit 1
fi

echo "--- Setup ---"
echo "Cleaning output..."
rm -rf "$OUT_DIR"
mkdir -p "$BIN_DIR"
mkdir -p "$ETC_DIR"

# 2. Compile Binary
echo "--- Building Binary ---"
echo "Compiling $BIN_NAME..."

$CC \
    --target=$TARGET \
    -O3 \
    -Wall \
    -o "$BIN_DIR/$BIN_NAME" \
    Main.c \
    -llog \
    -lm

if [ $? -eq 0 ]; then
    echo "✅ Build Successful: $BIN_DIR/$BIN_NAME"
else
    echo "❌ Build Failed"
    exit 1
fi

# 3. Generate RC File
echo "--- Generating Init Script ---"
# Updated service name to match the OBright convention
cat > "$ETC_DIR/$RC_NAME" <<EOF
service yamada_obright /vendor/bin/hw/$BIN_NAME
    class hal
    user system
    group system graphics
    capabilities SYS_NICE
    writepid /dev/cpuset/system-background/tasks

on property:sys.boot_completed=1
    # Change ownership strictly to the system user
    chown system system /sys/class/leds/lcd-backlight/brightness
    
    # Restrict write access to ONLY the owner (system)
    # 0600 means Read/Write for owner, No access for group/others
    chmod 0600 /sys/class/leds/lcd-backlight/brightness
    
    # Start service
    start yamada_obright
EOF

echo "✅ Created RC file: $ETC_DIR/$RC_NAME"

# 4. Finalize
echo "--- Done ---"
echo "Files are ready in '$OUT_DIR/vendor/'"
echo "To push to device:"
echo "  adb push $OUT_DIR/vendor/bin/hw/$BIN_NAME /vendor/bin/hw/"
echo "  adb push $OUT_DIR/vendor/etc/init/$RC_NAME /vendor/etc/init/"