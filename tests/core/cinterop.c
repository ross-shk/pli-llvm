/* Provides the external C procedure C_SET that cinterop.pli calls via its
   ENTRY declaration. PL/I passes x by reference, so this receives a pointer
   to the caller's variable. */
void C_SET(int *x) { *x = 42; }
