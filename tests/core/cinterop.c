/* Provides the external C procedure c_set that cinterop.pli calls via its
   ENTRY + EXTERNAL('c_set') declaration. PL/I passes x by reference, so
   this receives a pointer to the caller's variable. */
void c_set(int *x) { *x = 42; }
