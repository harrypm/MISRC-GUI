#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile the production reconnect selector with two same-name macOS cards."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
source = (root / "misrc_gui/core/misrc_gui.c").read_text()
start = source.index("static int gui_find_reconnect_device(")
opening = source.index("{", start)
depth = 1
end = opening + 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
function = source[start:end]
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
enum { DEVICE_TYPE_HSDAOH, DEVICE_TYPE_SIMPLE_CAPTURE, DEVICE_TYPE_CXADC,
       DEVICE_TYPE_MISRC_CLOCKGEN };
typedef struct { int type,index; char name[64],serial[64]; } device_info_t;
typedef struct { int device_count; device_info_t devices[2]; } gui_app_t;
typedef struct { bool valid; int type,index; char name[64],serial[64]; } gui_reconnect_target_t;
'''
checks = r'''
int main(void) {
    gui_app_t app={2,{
        {DEVICE_TYPE_CXADC,1,"identical CX23880","CXADC_MACOS_0_abc"},
        {DEVICE_TYPE_CXADC,1,"identical CX23880","CXADC_MACOS_1_def"}}};
    gui_reconnect_target_t target={true,DEVICE_TYPE_CXADC,1,"identical CX23880","CXADC_MACOS_1_def"};
    assert(gui_find_reconnect_device(&app,&target)==1);
    strcpy(target.serial,"CXADC_MACOS_0_abc");
    assert(gui_find_reconnect_device(&app,&target)==0);
    strcpy(target.serial,"CXADC_MACOS_1_replaced");
    assert(gui_find_reconnect_device(&app,&target)==-1);
    app.device_count=1;
    strcpy(target.serial,"CXADC_MACOS_1_def");
    assert(gui_find_reconnect_device(&app,&target)==-1);
    target.valid=false;
    assert(gui_find_reconnect_device(&app,&target)==-1);
    assert(gui_find_reconnect_device(0,&target)==-1);
    app.device_count=1;app.devices[0].index=2;
    strcpy(app.devices[0].name,"[CXADC] CXADC Clockgen");
    strcpy(app.devices[0].serial,"CXADC_MACOS_PAIR_abc_def");
    target.valid=true;target.index=2;
    strcpy(target.name,app.devices[0].name);strcpy(target.serial,app.devices[0].serial);
    assert(gui_find_reconnect_device(&app,&target)==0);
    strcpy(target.serial,"CXADC_MACOS_PAIR_abc_replaced");
    assert(gui_find_reconnect_device(&app,&target)==-1);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="cxadc-reconnect-") as directory:
    path = pathlib.Path(directory)
    (path / "test.c").write_text(fixture + function + checks)
    subprocess.run(["cc", "-std=c11", "-D__APPLE__", str(path / "test.c"),
                    "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("macOS CXADC reconnect selection: PASS")
