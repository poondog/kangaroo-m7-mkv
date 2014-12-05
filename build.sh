#!/bin/bash

# The font colors are predefined in this array. Color will choose one at random
FONT_COLORS=(31 32 33 34 35 36 91 92 93 94 95 96 97)
COLOR=$(($RANDOM % ${#FONT_COLORS[@]}))

echo -e "\e[${FONT_COLORS[$COLOR]}m "
echo "        _   __                                        "
echo "       | | / /                                        "
echo "       | |/ /  __ _ _ __   __ _  __ _ _ __ ___   ___  "
echo "       |    \ / _' | '_ \ / _' |/ _' | '__/ _ \ / _ \ "
echo "       | |\  \ (_| | | | | (_| | (_| | | | (_) | (_) |"
echo "       \_| \_/\__,_|_| |_|\__, |\__,_|_|  \___/ \___/ "
echo "                           __/ |                      "
echo "                          |___/                          _,'  ___         "
echo "                    _   __                     _       <__\__/   \        "
echo "                   | | / /                    | |         \_  /  _\       "
echo "                   | |/ /  ___ _ __ _ __   ___| |            \,\ / \\     "
echo "                   |    \ / _ \ '__| '_ \ / _ \ |              //   \\    "
echo "                   | |\  \  __/ |  | | | |  __/ |            ,/'     '\_, "
echo "                   \_| \_/\___|_|  |_| |_|\___|_|     "
echo "                                                      "
echo -e "\033[0m                                            "

VER="KANGAROO"

DATE_START=$(date +"%s")
export LOCALVERSION="-"`echo $VER`
export CROSS_COMPILE=${HOME}/Toolchains/arm-eabi-4.8-lollipop/bin/arm-eabi-

export ARCH=arm
export SUBARCH=arm
export KBUILD_BUILD_USER=poondoge

make "m7_defconfig"

make -j2

DATE_END=$(date +"%s")
DIFF=$(($DATE_END - $DATE_START))

echo
if (( $(($DIFF / 60)) == 0 )); then
	echo "  Build completed in $(($DIFF % 60)) seconds."
elif (( $(($DIFF / 60)) == 1 )); then
	echo "  Build completed in $(($DIFF / 60)) minute and $(($DIFF % 60)) seconds."
else
	echo "  Build completed in $(($DIFF / 60)) minutes and $(($DIFF % 60)) seconds."
fi
echo "  Finish time: $(date +"%r")"
echo
