# SPDX-License-Identifier: CC0-1.0
.class public Lcom/doublefine/cq/HostLoader;
.super Ljava/lang/Object;
.source "HostLoader.java"

# Loads libcqhost (the 64-bit host that runs the original ARM32 libraries).
# A copy in files/dev/ (pushed with adb run-as) takes precedence, for development.

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method public static load()V
    .registers 3
    new-instance v0, Ljava/io/File;
    const-string v1, "/data/data/com.doublefine.cq/files/dev/libcqhost.so"
    invoke-direct {v0, v1}, Ljava/io/File;-><init>(Ljava/lang/String;)V
    invoke-virtual {v0}, Ljava/io/File;->exists()Z
    move-result v2
    if-eqz v2, :use_packaged
    const-string v2, "cqhost"
    const-string v0, "loading development libcqhost from files/dev"
    invoke-static {v2, v0}, Landroid/util/Log;->i(Ljava/lang/String;Ljava/lang/String;)I
    invoke-static {v1}, Ljava/lang/System;->load(Ljava/lang/String;)V
    return-void
    :use_packaged
    const-string v1, "cqhost"
    invoke-static {v1}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V
    return-void
.end method
