#pragma once
#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include "WorkerRuntimeManifestAbi.h"
#include <cstdint>
#include <iterator>
#include <limits>
#include <vector>

namespace voice_studio::ipc
{
namespace runtime_manifest_detail
{
inline void appendU16(std::vector<UCHAR>& b, std::uint16_t v)
{ b.push_back(static_cast<UCHAR>(v)); b.push_back(static_cast<UCHAR>(v >> 8)); }
inline void appendU32(std::vector<UCHAR>& b, std::uint32_t v)
{ for (unsigned i=0;i<4;++i) b.push_back(static_cast<UCHAR>(v >> (i*8))); }
inline void appendU64(std::vector<UCHAR>& b, std::uint64_t v)
{ for (unsigned i=0;i<8;++i) b.push_back(static_cast<UCHAR>(v >> (i*8))); }
}

// Canonical stream: domain tag, fixed-width little-endian header fields,
// then ordered entry fields and UTF-16LE path bytes. Never hashes struct
// memory or padding. The digest field itself is excluded.
inline bool computeWorkerRuntimeManifestSha256(
    const WorkerRuntimeManifestHeader& header,
    const WorkerRuntimeManifestEntry* entries, std::size_t count,
    std::uint8_t (&output)[32]) noexcept
{
    if (!hasValidWorkerRuntimeManifestHeader(header) || entries == nullptr ||
        count != header.entryCount || !areWorkerRuntimeManifestPathsUnique(entries, count))
        return false;

    BCRYPT_ALG_HANDLE algorithm=nullptr;
    BCRYPT_HASH_HANDLE hash=nullptr;
    DWORD objectBytes=0, returned=0;
    std::vector<UCHAR> hashObject, canonical;
    bool ok=false;
    try
    {
        constexpr UCHAR domain[] = {'V','o','i','c','e','S','t','u','d','i','o','.',
            'R','u','n','t','i','m','e','M','a','n','i','f','e','s','t','.','v','1',0};
        canonical.reserve(sizeof(domain)+18+count*80);
        canonical.insert(canonical.end(),std::begin(domain),std::end(domain));
        runtime_manifest_detail::appendU32(canonical,header.magic);
        runtime_manifest_detail::appendU16(canonical,header.version);
        runtime_manifest_detail::appendU32(canonical,header.entryCount);
        runtime_manifest_detail::appendU64(canonical,header.totalBytes);
        for(std::size_t i=0;i<count;++i)
        {
            const auto& e=entries[i];
            if(!hasValidWorkerRuntimeManifestEntry(e)) return false;
            runtime_manifest_detail::appendU16(canonical,e.role);
            runtime_manifest_detail::appendU16(canonical,e.pathLength);
            runtime_manifest_detail::appendU64(canonical,e.sizeBytes);
            canonical.insert(canonical.end(),std::begin(e.sha256),std::end(e.sha256));
            for(std::size_t p=0;p<e.pathLength;++p)
                runtime_manifest_detail::appendU16(canonical,static_cast<std::uint16_t>(e.relativePath[p]));
        }
    }
    catch(...) { return false; }

    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) goto cleanup;
    if(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectBytes),
        sizeof(objectBytes),&returned,0)<0 || returned!=sizeof(objectBytes) || objectBytes==0) goto cleanup;
    try { hashObject.resize(objectBytes); } catch(...) { goto cleanup; }
    if(BCryptCreateHash(algorithm,&hash,hashObject.data(),objectBytes,nullptr,0,0)<0) goto cleanup;
    if(canonical.size()>static_cast<std::size_t>(std::numeric_limits<ULONG>::max()) ||
       BCryptHashData(hash,canonical.data(),static_cast<ULONG>(canonical.size()),0)<0) goto cleanup;
    if(BCryptFinishHash(hash,output,32,0)<0) goto cleanup;
    ok=true;
cleanup:
    if(hash) BCryptDestroyHash(hash);
    if(algorithm) BCryptCloseAlgorithmProvider(algorithm,0);
    return ok;
}

inline bool hasValidWorkerRuntimeManifestDigest(
    const WorkerRuntimeManifestHeader& header,
    const WorkerRuntimeManifestEntry* entries,std::size_t count) noexcept
{
    if(!hasValidWorkerRuntimeManifestEntries(header,entries,count)) return false;
    std::uint8_t calculated[32]{};
    if(!computeWorkerRuntimeManifestSha256(header,entries,count,calculated)) return false;
    std::uint8_t diff=0;
    for(std::size_t i=0;i<32;++i) diff|=static_cast<std::uint8_t>(calculated[i]^header.manifestSha256[i]);
    return diff==0;
}
} // namespace voice_studio::ipc
