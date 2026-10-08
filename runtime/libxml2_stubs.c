/*
 * Minimal libxml2 stubs for clang-cl + prebuilt LLVM 23 on Windows.
 *
 * The prebuilt LLVM links LLVMWindowsManifest (used by lld-COFF manifest
 * merging), which calls into libxml2 — but the prebuilt distribution ships
 * neither libxml2 headers nor binaries, and the previous import-lib stub
 * (deps/libxml2_stub) only re-exported 8 symbols while LLVM 23 needs 16,
 * besides requiring a libxml2.dll that does not exist at runtime.
 *
 * These stubs satisfy the link with static definitions so plic.exe starts
 * and runs without any libxml2 DLL. They abort with a clear message if
 * manifest merging is ever actually reached (only inputs carrying Windows
 * manifests take that path; ordinary PL/I console programs never do).
 *
 * Follow-up: build real libxml2 from source (iconv/icu/python off, static)
 * and link it instead, to enable manifest merging. See
 * docs/MIGRATION-CLANG-WINDOWS.md.
 */
#include <stdio.h>
#include <stdlib.h>

__declspec(noreturn) static void libxml2_missing(const char *fn) {
    fprintf(stderr, "plic: %s requires real libxml2 (Windows manifest merging is not stubbed)\n", fn);
    abort();
}

void xmlSetGenericErrorFunc(void *ctx, void *handler) {
    (void)ctx;
    (void)handler;
}
void *xmlReadMemory(const char *buffer, int size, const char *url,
                     const char *encoding, int options) {
    (void)buffer;
    (void)size;
    (void)url;
    (void)encoding;
    (void)options;
    libxml2_missing("xmlReadMemory");
    return NULL;
}
void *xmlDocGetRootElement(void *doc) {
    (void)doc;
    libxml2_missing("xmlDocGetRootElement");
    return NULL;
}
void xmlFreeDoc(void *doc) {
    (void)doc;
}
void xmlUnlinkNode(void *node) {
    (void)node;
}
void xmlFreeNode(void *node) {
    (void)node;
}
void *xmlNewProp(void *node, const unsigned char *name, const unsigned char *value) {
    (void)node;
    (void)name;
    (void)value;
    libxml2_missing("xmlNewProp");
    return NULL;
}
unsigned char *xmlStrdup(const unsigned char *str) {
    (void)str;
    libxml2_missing("xmlStrdup");
    return NULL;
}
void *xmlCopyNamespace(void *node, void *ns) {
    (void)node;
    (void)ns;
    libxml2_missing("xmlCopyNamespace");
    return NULL;
}
void xmlFree(void *ptr) {
    (void)ptr;
}
void *xmlAddChild(void *parent, void *child) {
    (void)parent;
    (void)child;
    libxml2_missing("xmlAddChild");
    return NULL;
}
void *xmlNewDoc(const unsigned char *version) {
    (void)version;
    libxml2_missing("xmlNewDoc");
    return NULL;
}
void *xmlDocSetRootElement(void *doc, void *root) {
    (void)doc;
    (void)root;
    libxml2_missing("xmlDocSetRootElement");
    return NULL;
}
int xmlDocDumpFormatMemoryEnc(void *doc, unsigned char **mem, int *size,
                               const char *encoding, int format) {
    (void)doc;
    (void)mem;
    (void)size;
    (void)encoding;
    (void)format;
    libxml2_missing("xmlDocDumpFormatMemoryEnc");
    return -1;
}
void xmlFreeNs(void *ns) {
    (void)ns;
}
void *xmlNewNs(void *node, const unsigned char *href, const unsigned char *prefix) {
    (void)node;
    (void)href;
    (void)prefix;
    libxml2_missing("xmlNewNs");
    return NULL;
}
