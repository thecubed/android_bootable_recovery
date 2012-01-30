/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "bootloader.h"
#include "common.h"
#include "firmware.h"
#include "roots.h"

#include <errno.h>
#include <string.h>
#include <sys/reboot.h>

#ifndef BOARD_RECOVERY_USES_HTC_FIRMWARE_ZIP
static const char *update_type = NULL;
static const char *update_data = NULL;
static int update_length = 0;
#else
static int update_pending = 0;
static htc_version_info htc_info;
#endif

#ifdef BOARD_RECOVERY_USES_HTC_FIRMWARE_ZIP
int get_htc_info() {
	memset(&htc_info, 0, sizeof(htc_info));
	htc_get_version_struct(&htc_info);
}

int htc_make_firmware_zip() {
	// Should call htc_write_android_info()
	// Then make the .zip file and put in proper place
	if (!htc_write_android_info()) {
		LOGI("Got android-info.txt file, making .zip file...\n");
		// make zip file!
		return 0;
	} else {
		LOGE("Failed making android-info.txt file, not writing zip file");
		return -1;
	}
}

int htc_write_android_info() {
	//if ( !(htc_info.mid == NULL || htc_info.cid == NULL || htc_info.mainver == NULL) ) { // TODO: Check if our required data is all there...
		FILE* androidinfo = fopen("/tmp/firmware/android-info.txt", "w");
		if (androidinfo == NULL) {
			LOGE("Error opening /tmp/firmware/android-info.txt for writing...");
			return -1;
		}
		fprintf(androidinfo, "modelid: %s\ncidnum: %s\nmainver: %s\nhbootpreupdate:12\nDelCache:0", htc_info.mid, htc_info.cid, htc_info.mainver);
		fclose(androidinfo);
		return 0;
	//} else {
	//	LOGE("Error creating android-info.txt, missing some data.");
	//	return -1;
	//}
}

#endif

int remember_firmware_update(const char *type, const char *data, int length) {
#ifndef BOARD_RECOVERY_USES_HTC_FIRMWARE_ZIP
    if (update_type != NULL || update_data != NULL) {
        LOGE("Multiple firmware images\n");
        return -1;
    }

    update_type = type;
    update_data = data;
    update_length = length;
    return 0;
#else
/* flow is:
	1. check if update_pending = 1
		if so, LOGE("Multiple firmware images... ignoring...");
	2. check type:
		if type = zip:
			copy data to /tmp/firmware/firmware.zip
			set update_pending = 1
		if type = bootimg
			copy file to /tmp/firmware/boot.img
			set update_pending = 1
	3. return properly
*/

	if (update_pending == 1) {
		LOGE("Multiple firmware images in queue. Denied!\n");
		return -1
	}

	if (strcmp(type, "zip") == 0) {
		LOGI("Got zip firmware image. Copying to /tmp/firmware/firmware.zip");
		// TODO: copy "data" to /tmp/firmware/firmware.zip
		update_pending = 1;
	} else if (strcmp(type, "bootimg") == 0) {
		LOGI("Got bootimg firmware image. Copying to /tmp/firmware/boot.img");
		// TODO: copy "data" to /tmp/firmware/boot.img
		update_pending = 1;
	} else {
		LOGE("Firmware type not supported, or missing type...\n");
		return -1;
	}

	return 0;

#endif
}


#ifdef BOARD_RECOVERY_USES_HTC_FIRMWARE_ZIP
// Return true if there is a firmware update pending.
int firmware_update_pending() {
	// TODO: check logic here...
	return update_pending;
}

int set_firmware_update_pending(int pending) {
	update_pending = pending;
}

#else

// Return true if there is a firmware update pending.
int firmware_update_pending() {
  return update_data != NULL && update_length > 0;
}
#endif

#ifdef BOARD_RECOVERY_USES_HTC_FIRMWARE_ZIP
/* M_I_F_U should now:
	1. if update-pending = 0, return 0
	2. if update-pending = 1, get_htc_info
	3. if /tmp/firmware.zip exists, copy to /sdcard/modelid.zip, else:
	4. if can't stat /tmp/firmware/android-info.txt, call htc_make_firmware_zip
	5. reboot to hboot (oem-42)
*/
int maybe_install_firmware_update(const char *send_intent) {
	if (update_pending == 0) {
		return 0;
	}
	// no need to worry about return values,
	// if the underlying functions that get this failed
	// the bootloader message check utilities would have ejected us already
	get_htc_info();
	
	// if /tmp/firmware/firmware.zip doesn't exist
	htc_make_firmware_zip();	

	// we don't return, we just reboot!
	// ...just kidding for now
	return 0;
}
#else
/* Bootloader / Recovery Flow
 *
 * On every boot, the bootloader will read the bootloader_message
 * from flash and check the command field.  The bootloader should
 * deal with the command field not having a 0 terminator correctly
 * (so as to not crash if the block is invalid or corrupt).
 *
 * The bootloader will have to publish the partition that contains
 * the bootloader_message to the linux kernel so it can update it.
 *
 * if command == "boot-recovery" -> boot recovery.img
 * else if command == "update-radio" -> update radio image (below)
 * else if command == "update-hboot" -> update hboot image (below)
 * else -> boot boot.img (normal boot)
 *
 * Radio/Hboot Update Flow
 * 1. the bootloader will attempt to load and validate the header
 * 2. if the header is invalid, status="invalid-update", goto #8
 * 3. display the busy image on-screen
 * 4. if the update image is invalid, status="invalid-radio-image", goto #8
 * 5. attempt to update the firmware (depending on the command)
 * 6. if successful, status="okay", goto #8
 * 7. if failed, and the old image can still boot, status="failed-update"
 * 8. write the bootloader_message, leaving the recovery field
 *    unchanged, updating status, and setting command to
 *    "boot-recovery"
 * 9. reboot
 *
 * The bootloader will not modify or erase the cache partition.
 * It is recovery's responsibility to clean up the mess afterwards.
 */

int maybe_install_firmware_update(const char *send_intent) {
    if (update_data == NULL || update_length == 0) return 0;

    /* We destroy the cache partition to pass the update image to the
     * bootloader, so all we can really do afterwards is wipe cache and reboot.
     * Set up this instruction now, in case we're interrupted while writing.
     */

    struct bootloader_message boot;
    memset(&boot, 0, sizeof(boot));
    strlcpy(boot.command, "boot-recovery", sizeof(boot.command));
    strlcpy(boot.recovery, "recovery\n--wipe_cache\n", sizeof(boot.command));
    if (send_intent != NULL) {
        strlcat(boot.recovery, "--send_intent=", sizeof(boot.recovery));
        strlcat(boot.recovery, send_intent, sizeof(boot.recovery));
        strlcat(boot.recovery, "\n", sizeof(boot.recovery));
    }
    if (set_bootloader_message(&boot)) return -1;

    int width = 0, height = 0, bpp = 0;
    char *busy_image = ui_copy_image(
        BACKGROUND_ICON_FIRMWARE_INSTALLING, &width, &height, &bpp);
    char *fail_image = ui_copy_image(
        BACKGROUND_ICON_FIRMWARE_ERROR, &width, &height, &bpp);

    ui_print("Writing %s image...\n", update_type);
    if (write_update_for_bootloader(
            update_data, update_length,
            width, height, bpp, busy_image, fail_image)) {
        LOGE("Can't write %s image\n(%s)\n", update_type, strerror(errno));
        format_volume("/cache");  // Attempt to clean cache up, at least.
        return -1;
    }

    free(busy_image);
    free(fail_image);

    /* The update image is fully written, so now we can instruct the bootloader
     * to install it.  (After doing so, it will come back here, and we will
     * wipe the cache and reboot into the system.)
     */
    snprintf(boot.command, sizeof(boot.command), "update-%s", update_type);
    if (set_bootloader_message(&boot)) {
        format_volume("/cache");
        return -1;
    }

    reboot(RB_AUTOBOOT);

    // Can't reboot?  WTF?
    LOGE("Can't reboot\n");
    return -1;
}
#endif
