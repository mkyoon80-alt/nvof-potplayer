#include <bcrypt.h>
#include <iomanip>
#include <sstream>
std::string hash_bytes(const std::vector<uint8_t>& bytes){
    BCRYPT_ALG_HANDLE alg=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
    require(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,"SHA provider");
    require(BCryptCreateHash(alg,&hash,nullptr,0,nullptr,0,0)>=0,"SHA create");
    require(BCryptHashData(hash,const_cast<PUCHAR>(bytes.data()),ULONG(bytes.size()),0)>=0,"SHA data");
    unsigned char digest[32]{};require(BCryptFinishHash(hash,digest,32,0)>=0,"SHA finish");
    BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(alg,0);
    std::ostringstream s;for(auto b:digest)s<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(b);return s.str();
}
