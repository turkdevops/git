@@
identifier f != git_hash_init;
expression ALGO;
struct git_hash_ctx *CTX;
@@
  f(...) {<...
- ALGO->init_fn(CTX);
+ git_hash_init(CTX, ALGO);
  ...>}
