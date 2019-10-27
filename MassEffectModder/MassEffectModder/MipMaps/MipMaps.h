/*
 * MassEffectModder
 *
 * Copyright (C) 2018-2019 Pawel Kolodziejski
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */

#ifndef MIPMAPS_H
#define MIPMAPS_H

#include <Types/MemTypes.h>
#include <Texture/Texture.h>
#include <Texture/TextureScan.h>
#include <Image/Image.h>
#include <Helpers/ByteBuffer.h>
#include <Helpers/Stream.h>

struct FileMod
{
    uint tag;
    QString name;
    qint64 offset;
    qint64 size;
};

struct RemoveMipsEntry
{
    QString pkgPath;
    QList<int> exportIDs;
};

struct TFCTexture
{
    quint8 guid[16];
    QString name;
};

struct ModEntry
{
    QString packagePath;
    int exportId;

    QString textureName;
    uint textureCrc;
    bool markConvert;
    QList<MipMap> cacheCprMipmaps;
    Texture::StorageTypes cacheCprMipmapsStorageType;
    quint64 cacheSize;
    QList<int> cacheCprMipmapsDecompressedSize;
    PixelFormat cachedPixelFormat;
    QMap<int, QList<Texture::TextureMipMap>> masterTextures;
    QList<Texture::TextureMipMap> arcTexture;
    quint8 arcTfcGuid[16];
    QString arcTfcName;
    bool arcTfcDLC;
    int instance;

    bool binaryModType;
    ByteBuffer binaryModData;

    QString memPath;
    quint64 memEntryOffset;
    long memEntrySize;

    void CopyMipMapsList(QList<Texture::TextureMipMap> &copy,
                         const QList<Texture::TextureMipMap> &list)
    {
        foreach(Texture::TextureMipMap mip, list)
        {
            Texture::TextureMipMap newMip = mip;
            newMip.freeNewData = false;
            copy.append(newMip);
        }
    }
};

struct MapTexturesToMod
{
    QString packagePath;
    int modIndex;
    int texturesIndex;
    int listIndex;
};

struct MapPackagesToModEntry
{
    int modIndex;
    int texturesIndex;
    int listIndex;
};

struct MapPackagesToMod
{
    QString packagePath;
    QList<MapPackagesToModEntry> textures;
    long usage;
    int instances;
    long weight;
    bool slave;
    RemoveMipsEntry removeMips;
};

class MipMaps
{
    enum ChunkBlockEnums
    {
        SizeOfChunkBlock = 8,
        SizeOfChunk = 8,
        maxBlockSize = 0x20000, // 128KB
    };

    void prepareListToRemove(QList<FoundTexture> &textures, QList<RemoveMipsEntry> &list);

public:

    typedef void (*ProgressCallback)(void *handle, int progress, const QString &stage);

    static bool compressData(ByteBuffer inputData, Stream &ouputStream);
    static ByteBuffer decompressData(Stream &stream, long compressedSize);
    void extractTextureToPng(QString &outputFile, QString &packagePath, int exportID);

    void removeMipMaps(int phase, QList<FoundTexture> &textures, QStringList &pkgsToMarker,
                       QStringList &pkgsToRepack, bool repack, bool appendMarker,
                       ProgressCallback callback, void *callbackHandle);
    void removeMipMapsPerPackage(int phase, QList<FoundTexture> &textures, Package &package,
                                 RemoveMipsEntry &removeEntry,
                                 QStringList &pkgsToMarker, QStringList &pkgsToRepack,
                                 bool repack, bool appendMarker);

    PixelFormat changeTextureType(PixelFormat gamePixelFormat, PixelFormat texturePixelFormat,
                                  Texture &texture);
    bool VerifyTextures(QList<FoundTexture> &textures,
                        ProgressCallback callback, void *callbackHandle);
    QString replaceTextures(QList<MapPackagesToMod> &map, QList<FoundTexture> &textures,
                            QStringList &pkgsToMarker, QStringList &pkgsToRepack,
                            QList<ModEntry> &modsToReplace,
                            bool repack, bool appendMarker, bool verify,
                            bool removeMips, int cacheAmount,
                            ProgressCallback callback, void *callbackHandle);
    QString replaceModsFromList(QList<FoundTexture> &textures, QStringList &pkgsToMarker,
                                QStringList &pkgsToRepack,QList<ModEntry> &modsToReplace, bool repack,
                                bool appendMarker, bool verify, bool removeMips, int cacheAmount,
                                ProgressCallback callback, void *callbackHandle);
    static void RemoveLowerMips(Image *image, Texture *texture);
    static void AddMissingLowerMips(Image *image, Texture *texture);
    static void RemoveLowerMips(Image *image);
};

#endif