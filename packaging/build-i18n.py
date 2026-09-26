#!/usr/bin/env python3
# Build gui-tauri/ui/i18n.js from Rufus's own .po files plus Rufux extras.
# Political choice (maintainer's): he-IL and fa-IR are excluded, everything
# else Rufus ships is included. Missing strings fall back to English;
# Arabic gaps are hand-filled below (AR_EXTRA).
import os, re, json, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PO_DIR = "/home/hultwl/Projects/rufus/res/loc/po"
LOC = "/home/hultwl/Projects/rufus/res/loc/rufus.loc"
OUT = os.path.join(ROOT, "gui-tauri", "ui", "i18n.js")

SKIP = {"he-IL", "fa-IR"}
RTL = {"ar-SA"}

# key: (rufus msgid or None, case-insensitive match ok, upper?)
STRINGS = [
    ("drive_props", "Drive Properties", False),
    ("device", "Device", False),
    ("no_usb", None, False),
    ("boot_sel", "Boot selection", False),
    ("nonboot", "Non bootable", False),
    ("freedos", None, False),
    ("iso_please", "Disk or ISO image (Please select)", False),
    ("select", "Select", True),
    ("download", "Download", True),
    ("img_opt", "Image Option", False),
    ("std_win", "Standard Windows installation", False),
    ("persist", "Persistent partition size", False),
    ("gb", None, False),
    ("mb", None, False),
    ("scheme", "Partition scheme", False),
    ("mbr", None, False),
    ("gpt", None, False),
    ("target", "Target system", False),
    ("bios_csm", "BIOS (or UEFI-CSM)", False),
    ("uefi_nocsm", "UEFI (non CSM)", False),
    ("show_tpl", "Show %s", False),
    ("hide_tpl", "Hide %s", False),
    ("adv_drive_noun", "advanced drive properties", False),
    ("adv_fmt_noun", "advanced format options", False),
    ("list_hdd", "List USB Hard Drives", False),
    ("old_bios", "Add fixes for old BIOSes (extra partition, align, etc.)", False),
    ("bios_id", None, False),
    ("bios_id_val", None, False),
    ("uefi_valid", "Enable runtime UEFI media validation", False),
    ("fmt_opts", "Format Options", False),
    ("vol_label", "Volume label", False),
    ("fs", "File system", False),
    ("large_fat32", None, False),
    ("fat32", None, False),
    ("fat16", None, False),
    ("ntfs", None, False),
    ("exfat", None, False),
    ("udf", None, False),
    ("ext2", None, False),
    ("ext3", None, False),
    ("ext4", None, False),
    ("cluster", "Cluster size", False),
    ("cl_512", None, False),
    ("cl_1024", None, False),
    ("cl_2048", None, False),
    ("cl_4096", None, False),
    ("cl_8192", None, False),
    ("cl_16k", None, False),
    ("cl_32k", None, False),
    ("cl_64k", None, False),
    ("quick", "Quick format", False),
    ("ext_label", "Create extended label and icon files", False),
    ("bad_blocks", "Check device for bad blocks", False),
    ("pass1", None, False),
    ("pass2", None, False),
    ("pass3", None, False),
    ("pass4", None, False),
    ("status", "Status", False),
    ("ready", "READY", False),
    ("start", "Start", True),
    ("close", "Close", False),
    ("wue_title", "Windows User Experience", False),
    ("customize", "Customize Windows installation?", False),
    ("wue_bypass", "Remove requirement for 4GB+ RAM, Secure Boot and TPM 2.0", False),
    ("wue_nro", "Remove requirement for an online Microsoft account", False),
    ("wue_user", "Create a local account with username:", False),
    ("wue_locale", "Set regional options to the same values as this user's", False),
    ("wue_privacy", "Disable data collection (Skip privacy questions)", False),
    ("wue_bitlocker", "Disable BitLocker automatic device encryption", False),
    ("wue_qol", None, False),
    ("cancel", "Cancel", False),
    ("cancel_op", "Cancel", True),
    ("ok", "OK", False),
    ("dl_title", None, False),
    ("version", "Version", False),
    ("win11", None, False),
    ("win10", None, False),
    ("edition", "Edition", False),
    ("language", "Language", False),
    ("arch", "Architecture", False),
    ("arch_def", None, False),
    ("dl_hint", None, False),
    ("download_btn", "Download", False),
    ("log", "Log", False),
    ("clear", "Clear", False),
    ("save", "Save", False),
    ("hash_title", None, False),
    ("computing", None, False),
    ("cancel_op", "Cancel", True),
    ("sel_img", None, False),
    ("enter_user", None, False),
    ("save_unsupported", None, False),
    ("about_text", None, False),
    ("updates_pkg", None, False),
    ("cannot_use_img", None, False),
    ("cannot_start", None, False),
    ("save_failed", None, False),
    ("startup_failed", None, False),
    ("auth_cancel", None, False),
    ("worker_exited", None, False),
    ("dev1", "%d device found", False),
    ("devN", "%d devices found", False),
    ("devN_ar2", None, False),
    ("devN_ar310", None, False),
    ("devN_ar11", None, False),
    ("reading", None, False),
    ("dl_listing", None, False),
    ("dl_pick", None, False),
    ("dl_langs", None, False),
    ("dl_failed", None, False),
    ("dl_starting", None, False),
    ("iso_msg", "The image you have selected is an 'ISOHybrid' image. This means it can be written either in %s (file copy) mode or %s (disk image) mode.\nRufus recommends using %s mode, so that you always have full access to the drive after writing it.\nHowever, if you encounter issues during boot, you can try writing this image again in %s mode.\n\nPlease select the mode that you want to use to write this image:", False),
    ("destroy_msg", "WARNING: ALL DATA ON DEVICE '%s' WILL BE DESTROYED.\nTo continue with this operation, click OK. To quit click CANCEL.", False),
    ("iso_btn", None, False),
    ("dd_btn", None, False),
    ("tip_hash", None, False),
    ("tip_save", None, False),
    ("tip_about", None, False),
    ("tip_log", None, False),
    ("tip_settings", None, False),
    ("iso_please", "Disk or ISO image (Please select)", False),
    ("tip_lang", None, False),
    ("tip_biosid", None, False),
    ("def_tpl", "%s (Default)", False),
    ("dl_done", None, False),
    ("dl_list_fail", None, False),
    ("dl_lang_fail", None, False),
    ("close_btn", "Close", True),
]

# English source for Rufux-specific strings (msgid None) and %s templates.
EN = {
    "no_usb": "No USB drive found",
    "freedos": "FreeDOS",
    "gb": "GB", "mb": "MB", "mbr": "MBR", "gpt": "GPT",
    "fat32": "FAT32", "fat16": "FAT16", "ntfs": "NTFS", "exfat": "exFAT",
    "udf": "UDF", "ext2": "ext2", "ext3": "ext3", "ext4": "ext4",
    "large_fat32": "Large FAT32",
    "bios_id": "Use Rufus MBR with BIOS ID",
    "bios_id_val": "0x80 (Default)",
    "cl_512": "512 bytes", "cl_1024": "1024 bytes", "cl_2048": "2048 bytes",
    "cl_4096": "4096 bytes (Default)", "cl_8192": "8192 bytes",
    "cl_16k": "16 kilobytes", "cl_32k": "32 kilobytes", "cl_64k": "64 kilobytes",
    "pass1": "1 pass", "pass2": "2 passes", "pass3": "3 passes", "pass4": "4 passes",
    "hash_title": "Checksums",
    "computing": "Computing…",
    "wue_qol": "QoL improvements (Don't force Copilot, OneDrive, Outlook, Fast Startup, etc.)",
    "dl_title": "Download Windows ISO",
    "win11": "Windows 11", "win10": "Windows 10",
    "arch_def": "Default (x64)",
    "dl_hint": "The list comes from Microsoft's download service. Pick edition, language, then Download.",
    "sel_img": "Please select a disk or ISO image.",
    "enter_user": "Please enter a user name.",
    "save_unsupported": "Saving drive contents is not supported by the Linux backend yet.",
    "about_text": "Rufux 2.1.0\nCreate bootable USB drives on Linux.\nA Linux port of Rufus by Pete Batard. License: GPLv3.\nhttps://github.com/Hultwl/Rufux",
    "updates_pkg": "Updates are handled by your package manager.",
    "cannot_use_img": "Cannot use image:\n",
    "cannot_start": "Cannot start:\n",
    "save_failed": "Save failed: ",
    "startup_failed": "Startup failed: ",
    "auth_cancel": "Authorization was cancelled or pkexec is unavailable.",
    "worker_exited": "The worker exited with code %d.",
    "devN_ar2": "%d devices found",
    "devN_ar310": "%d devices found",
    "devN_ar11": "%d devices found",
    "reading": "Reading image…",
    "dl_listing": "Asking Microsoft for the product list…",
    "dl_pick": "Pick edition, language, then Download.",
    "dl_langs": "Asking Microsoft for languages…",
    "dl_failed": "Download failed (code %d).",
    "dl_starting": "Starting download…",
    "iso_msg": "The image you have selected is an 'ISOHybrid' image. This means it can be written either in ISO Image (file copy) mode or DD Image (disk image) mode.\nRufux recommends using ISO Image mode, so that you always have full access to the drive after writing it.\nHowever, if you encounter issues during boot, you can try writing this image again in DD Image mode.\n\nPlease select the mode that you want to use to write this image:",
    "destroy_msg": "WARNING: ALL DATA ON DEVICE '%s' WILL BE DESTROYED.\nTo continue with this operation, click OK. To quit click CANCEL.",
    "iso_btn": "Write in ISO Image mode (Recommended)",
    "dd_btn": "Write in DD Image mode",
    "tip_hash": "Compute the checksums of the image",
    "tip_save": "Save the drive contents to a file",
    "tip_about": "About Rufux",
    "tip_log": "Show the log",
    "tip_settings": "Settings",
    "tip_lang": "Language",
    "tip_biosid": "Not supported by the Linux backend yet",
    "dl_done": "Done: ",
    "dl_list_fail": "Download list failed: ",
    "dl_lang_fail": "Language list failed: ",
}

# Hand-written Arabic for Rufux-specific strings (MSA).
AR_EXTRA = {
    "no_usb": "لم يتم العثور على محرك أقراص USB",
    "dev1": "تم العثور على جهاز واحد",
    "iso_please": "قرص أو صورة ISO (الرجاء الاختيار)",
    "bios_id": "استخدام MBR الخاص بـ Rufus مع معرّف BIOS",
    "bios_id_val": "0x80 (الافتراضي)",
    "large_fat32": "FAT32 الكبير",
    "cl_512": "512 بايت", "cl_1024": "1024 بايت", "cl_2048": "2048 بايت",
    "cl_4096": "4096 بايت (الافتراضي)", "cl_8192": "8192 بايت",
    "cl_16k": "16 كيلوبايت", "cl_32k": "32 كيلوبايت", "cl_64k": "64 كيلوبايت",
    "pass1": "مرة واحدة", "pass2": "مرتين", "pass3": "3 مرات", "pass4": "4 مرات",
    "hash_title": "مجاميع التحقق",
    "computing": "جارٍ الحساب…",
    "wue_qol": "تحسينات جودة الحياة (عدم فرض Copilot وOneDrive وOutlook والتشغيل السريع، إلخ.)",
    "dl_title": "تنزيل ISO ويندوز",
    "arch_def": "الافتراضي (x64)",
    "dl_hint": "القائمة من خدمة التنزيل لدى Microsoft. اختر الطبعة واللغة ثم تنزيل.",
    "sel_img": "الرجاء اختيار قرص أو صورة ISO.",
    "enter_user": "الرجاء إدخال اسم مستخدم.",
    "save_unsupported": "حفظ محتويات المحرك غير مدعوم في نسخة لينكس بعد.",
    "about_text": "Rufux 2.1.0\nلإنشاء محركات USB قابلة للإقلاع على لينكس.\nنسخة لينكس من Rufus للمطور Pete Batard. الرخصة: GPLv3.\nhttps://github.com/Hultwl/Rufux",
    "updates_pkg": "التحديثات عبر مدير الحزم في نظامك.",
    "cannot_use_img": "تعذر استخدام الصورة:\n",
    "cannot_start": "تعذر البدء:\n",
    "save_failed": "فشل الحفظ: ",
    "startup_failed": "فشل بدء التشغيل: ",
    "auth_cancel": "تم إلغاء التفويض أو pkexec غير متوفر.",
    "worker_exited": "خرج العامل بالرمز %d.",
    "dev1": "تم العثور على جهاز واحد",
    "devN": "%d جهازًا",
    "devN_ar2": "تم العثور على جهازين",
    "devN_ar310": "تم العثور على %d أجهزة",
    "devN_ar11": "تم العثور على %d جهازًا",
    "reading": "جارٍ قراءة الصورة…",
    "dl_listing": "جارٍ سؤال Microsoft عن قائمة المنتجات…",
    "dl_pick": "اختر الطبعة واللغة ثم تنزيل.",
    "dl_langs": "جارٍ سؤال Microsoft عن اللغات…",
    "dl_failed": "فشل التنزيل (الرمز %d).",
    "dl_starting": "جارٍ بدء التنزيل…",
    "iso_btn": "الكتابة في وضع صورة ISO (مُستحسَن)",
    "dd_btn": "الكتابة في وضع صورة DD",
    "tip_hash": "حساب مجاميع التحقق للصورة",
    "tip_save": "حفظ محتويات المحرك في ملف",
    "tip_about": "حول Rufux",
    "tip_log": "إظهار السجل",
    "tip_settings": "الإعدادات",
    "tip_lang": "اللغة",
    "tip_biosid": "غير مدعوم في نسخة لينكس بعد",
    "dl_done": "تم: ",
    "dl_list_fail": "فشل جلب القائمة: ",
    "dl_lang_fail": "فشل جلب اللغات: ",
    "ready": "جاهز.",
}

TEMPLATE_KEYS = {"iso_msg", "destroy_msg", "worker_exited", "dl_failed", "dev1", "devN",
                 "devN_ar2", "devN_ar310", "devN_ar11", "show_tpl", "hide_tpl"}


def load_po(path):
    d = {}
    cur_id = cur_str = None
    in_id = in_str = False

    def q(s):
        try:
            return eval(s)
        except Exception:
            return s.strip('"')

    def flush():
        nonlocal cur_id, cur_str
        if cur_id is not None:
            d[cur_id] = cur_str or ""
        cur_id, cur_str = None, None

    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("msgid "):
                flush()
                cur_id = q(line[6:])
                in_id, in_str = True, False
            elif line.startswith("msgstr "):
                cur_str = q(line[7:])
                in_id, in_str = False, True
            elif line.startswith('"'):
                if in_id:
                    cur_id += q(line)
                elif in_str:
                    cur_str += q(line)
            elif line.strip() == "":
                flush()
                in_id, in_str = False, False
    flush()
    return d


def lang_list():
    langs = []
    for line in open(LOC, encoding="utf-8", errors="replace"):
        m = re.match(r'# • v[\d.]+ "([a-zA-Z-]+)" "(.+)"', line)
        if m:
            code, name = m.group(1), m.group(2)
            if code in SKIP or code == "en-US":
                continue
            if os.path.exists(os.path.join(PO_DIR, code + ".po")):
                langs.append((code, name))
    return langs


def main():
    langs = lang_list()
    print(f"languages (minus {sorted(SKIP)}): {len(langs)}")
    out = {"_meta": {}}
    # English baseline
    en = {}
    for key, msgid, upper in STRINGS:
        en[key] = EN.get(key, msgid if msgid else key)
    out["en-US"] = en
    out["_meta"]["en-US"] = {"name": "English (English)", "rtl": False}

    for code, name in langs:
        po = load_po(os.path.join(PO_DIR, code + ".po"))
        lower = {k.lower(): v for k, v in po.items()}
        d = {}
        hits = 0
        for key, msgid, upper in STRINGS:
            val = ""
            if code == "ar-SA" and key in AR_EXTRA:
                val = AR_EXTRA[key]
            if not val and msgid:
                val = po.get(msgid, "")
                if not val:
                    hit = lower.get(msgid.lower(), "")
                    val = hit if isinstance(hit, str) else ""
            if not val and code == "ar-SA":
                val = AR_EXTRA.get(key, "")
            if val:
                hits += 1
            d[key] = val  # empty = fallback to English in JS
        # Arabic extras for keys whose msgid exists in po but ar translation empty
        if code == "ar-SA":
            for key, msgid, upper in STRINGS:
                if not d[key] and key in AR_EXTRA:
                    d[key] = AR_EXTRA[key]
                    hits += 1
        out[code] = d
        out["_meta"][code] = {"name": name, "rtl": code in RTL}
        print(f"  {code:8s} {hits}/{len(STRINGS)}")
    out["_keys_upper"] = [k for k, m, u in STRINGS if u]
    out["_templates"] = sorted(TEMPLATE_KEYS)

    js = "// Generated by packaging/build-i18n.py from Rufus's .po files + Rufux extras.\n"
    js += "// he-IL and fa-IR are excluded by maintainer choice. Empty string = English fallback.\n"
    js += "window.RUFUX_I18N = " + json.dumps(out, ensure_ascii=False, indent=1) + ";\n"
    open(OUT, "w", encoding="utf-8").write(js)
    print("wrote", OUT, os.path.getsize(OUT) // 1024, "KB")


if __name__ == "__main__":
    main()
