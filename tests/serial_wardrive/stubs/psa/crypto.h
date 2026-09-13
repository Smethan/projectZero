#pragma once
#include <openssl/sha.h>
typedef SHA256_CTX psa_hash_operation_t;
#define PSA_HASH_OPERATION_INIT {0}
#define PSA_ALG_SHA_256 1
static int psa_hash_setup(psa_hash_operation_t *c,int alg){return SHA256_Init(c)==1?0:1;}
static int psa_hash_update(psa_hash_operation_t *c,const unsigned char *data,size_t size){return SHA256_Update(c,data,size)==1?0:1;}
static int psa_hash_finish(psa_hash_operation_t *c,unsigned char *out,size_t capacity,size_t *size){*size=32;return SHA256_Final(out,c)==1?0:1;}
static void psa_hash_abort(psa_hash_operation_t *c){}
