cp -r ../flash0 assets
cp -r ../lang assets
cp -r ../assets/shaders assets
cp ../assets/langregion.ini assets/langregion.ini
NDK_MODULE_PATH=..:../native/ext $NDK/ndk-build -j3 TARGET_PLATFORM=android-9 $*
