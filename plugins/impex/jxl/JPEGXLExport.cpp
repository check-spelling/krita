/*
 *  SPDX-FileCopyrightText: 2021 the JPEG XL Project Authors
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  SPDX-FileCopyrightText: 2022 L. E. Segovia <amy@amyspark.me>
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "JPEGXLExport.h"

#include <jxl/color_encoding.h>
#include <jxl/encode_cxx.h>
#include <jxl/resizable_parallel_runner_cxx.h>
#include <kpluginfactory.h>

#include <QBuffer>
#include <cstdint>

#include <KisDocument.h>
#include <KisExportCheckRegistry.h>
#include <KisImportExportErrorCode.h>
#include <KoAlwaysInline.h>
#include <KoColorModelStandardIds.h>
#include <KoColorProfile.h>
#include <KoColorSpace.h>
#include <KoColorTransferFunctions.h>
#include <KoConfig.h>
#include <kis_assert.h>
#include <kis_debug.h>
#include <kis_exif_info_visitor.h>
#include <kis_image_animation_interface.h>
#include <kis_iterator_ng.h>
#include <kis_layer.h>
#include <kis_layer_utils.h>
#include <kis_meta_data_backend_registry.h>
#include <kis_raster_keyframe_channel.h>
#include <kis_time_span.h>

#include "kis_wdg_options_jpegxl.h"

K_PLUGIN_FACTORY_WITH_JSON(ExportFactory, "krita_jxl_export.json", registerPlugin<JPEGXLExport>();)

namespace HDR
{
template<ConversionPolicy policy>
ALWAYS_INLINE float applyCurveAsNeeded(float value)
{
    if (policy == ConversionPolicy::ApplyPQ) {
        return applySmpte2048Curve(value);
    } else if (policy == ConversionPolicy::ApplyHLG) {
        return applyHLGCurve(value);
    } else if (policy == ConversionPolicy::ApplySMPTE428) {
        return applySMPTE_ST_428Curve(value);
    }
    return value;
}

template<typename CSTrait,
         bool swap,
         bool convertToRec2020,
         bool isLinear,
         ConversionPolicy conversionPolicy,
         typename DestTrait,
         bool removeOOTF>
inline QByteArray writeLayer(const int width,
                             const int height,
                             KisHLineConstIteratorSP it,
                             float hlgGamma,
                             float hlgNominalPeak,
                             const KoColorSpace *cs)
{
    const int channels = static_cast<int>(CSTrait::channels_nb);
    QVector<float> pixelValues(channels);
    QVector<qreal> pixelValuesLinear(channels);
    const KoColorProfile *profile = cs->profile();
    const QVector<qreal> lCoef = cs->lumaCoefficients();
    double *src = pixelValuesLinear.data();
    float *dst = pixelValues.data();

    QByteArray res;
    res.resize(width * height * static_cast<int>(DestTrait::pixelSize));

    quint8 *ptr = reinterpret_cast<quint8 *>(res.data());

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            CSTrait::normalisedChannelsValue(it->rawDataConst(), pixelValues);
            if (!convertToRec2020 && !isLinear) {
                for (int i = 0; i < channels; i++) {
                    src[i] = static_cast<double>(dst[i]);
                }
                profile->linearizeFloatValue(pixelValuesLinear);
                for (int i = 0; i < channels; i++) {
                    dst[i] = static_cast<float>(src[i]);
                }
            }

            if (conversionPolicy == ConversionPolicy::ApplyHLG && removeOOTF) {
                removeHLGOOTF(dst, lCoef.constData(), hlgGamma, hlgNominalPeak);
            }

            for (int ch = 0; ch < channels; ch++) {
                if (ch == CSTrait::alpha_pos) {
                    dst[ch] = applyCurveAsNeeded<ConversionPolicy::KeepTheSame>(
                        dst[ch]);
                } else {
                    dst[ch] = applyCurveAsNeeded<conversionPolicy>(dst[ch]);
                }
            }

            if (swap) {
                std::swap(dst[0], dst[2]);
            }

            DestTrait::fromNormalisedChannelsValue(ptr, pixelValues);

            ptr += DestTrait::pixelSize;

            it->nextPixel();
        }

        it->nextRow();
    }

    return res;
}

template<typename CSTrait, bool swap>
inline QByteArray writeLayerNoConversion(const int width,
                                         const int height,
                                         KisHLineConstIteratorSP it,
                                         float hlgGamma,
                                         float hlgNominalPeak,
                                         const KoColorSpace *cs)
{
    Q_UNUSED(hlgGamma);
    Q_UNUSED(hlgNominalPeak);
    Q_UNUSED(cs);

    const int channels = static_cast<int>(CSTrait::channels_nb);
    QVector<float> pixelValues(channels);
    QVector<qreal> pixelValuesLinear(channels);

    QByteArray res;
    res.resize(width * height * static_cast<int>(CSTrait::pixelSize));

    quint8 *ptr = reinterpret_cast<quint8 *>(res.data());

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            auto *dst = reinterpret_cast<typename CSTrait::channels_type *>(ptr);

            std::memcpy(dst, it->rawDataConst(), CSTrait::pixelSize);

            if (swap) {
                std::swap(dst[0], dst[2]);
            }

            ptr += CSTrait::pixelSize;

            it->nextPixel();
        }

        it->nextRow();
    }

    return res;
}

template<typename CSTrait,
         bool swap,
         bool convertToRec2020,
         bool isLinear,
         ConversionPolicy linearizePolicy,
         typename DestTrait,
         bool removeOOTF,
         typename... Args>
ALWAYS_INLINE auto writeLayerSimplify(Args &&...args)
{
    if (linearizePolicy != ConversionPolicy::KeepTheSame) {
        return writeLayer<CSTrait, swap, convertToRec2020, isLinear, linearizePolicy, DestTrait, removeOOTF>(
            std::forward<Args>(args)...);
    } else {
        return writeLayerNoConversion<CSTrait, swap>(std::forward<Args>(args)...);
    }
}

template<typename CSTrait,
         bool swap,
         bool convertToRec2020,
         bool isLinear,
         ConversionPolicy linearizePolicy,
         typename DestTrait,
         typename... Args>
ALWAYS_INLINE auto writeLayerWithPolicy(bool removeOOTF, Args &&...args)
{
    if (removeOOTF) {
        return writeLayerSimplify<CSTrait, swap, convertToRec2020, isLinear, linearizePolicy, DestTrait, true>(
            std::forward<Args>(args)...);
    } else {
        return writeLayerSimplify<CSTrait, swap, convertToRec2020, isLinear, linearizePolicy, DestTrait, false>(
            std::forward<Args>(args)...);
    }
}

template<typename CSTrait, bool swap, bool convertToRec2020, bool isLinear, typename... Args>
ALWAYS_INLINE auto writeLayerWithLinear(ConversionPolicy linearizePolicy, Args &&...args)
{
    if (linearizePolicy == ConversionPolicy::ApplyHLG) {
        return writeLayerWithPolicy<CSTrait,
                                    swap,
                                    convertToRec2020,
                                    isLinear,
                                    ConversionPolicy::ApplyHLG,
                                    KoBgrU16Traits>(std::forward<Args>(args)...);
    } else if (linearizePolicy == ConversionPolicy::ApplyPQ) {
        return writeLayerWithPolicy<CSTrait,
                                    swap,
                                    convertToRec2020,
                                    isLinear,
                                    ConversionPolicy::ApplyPQ,
                                    KoBgrU16Traits>(std::forward<Args>(args)...);
    } else if (linearizePolicy == ConversionPolicy::ApplySMPTE428) {
        return writeLayerWithPolicy<CSTrait,
                                    swap,
                                    convertToRec2020,
                                    isLinear,
                                    ConversionPolicy::ApplySMPTE428,
                                    KoBgrU16Traits>(std::forward<Args>(args)...);
    } else {
        return writeLayerWithPolicy<CSTrait, swap, convertToRec2020, isLinear, ConversionPolicy::KeepTheSame, CSTrait>(
            std::forward<Args>(args)...);
    }
}

template<typename CSTrait, bool swap, bool convertToRec2020, typename... Args>
ALWAYS_INLINE auto writeLayerWithRec2020(bool isLinear, Args &&...args)
{
    if (isLinear) {
        return writeLayerWithLinear<CSTrait, swap, convertToRec2020, true>(std::forward<Args>(args)...);
    } else {
        return writeLayerWithLinear<CSTrait, swap, convertToRec2020, false>(std::forward<Args>(args)...);
    }
}

template<typename CSTrait, bool swap, typename... Args>
ALWAYS_INLINE auto writeLayerWithSwap(bool convertToRec2020, Args &&...args)
{
    if (convertToRec2020) {
        return writeLayerWithRec2020<CSTrait, swap, true>(std::forward<Args>(args)...);
    } else {
        return writeLayerWithRec2020<CSTrait, swap, false>(std::forward<Args>(args)...);
    }
}

template<typename... Args>
inline auto writeLayer(const KoID &id, Args &&...args)
{
    if (id == Integer8BitsColorDepthID) {
        return writeLayerWithSwap<KoBgrU8Traits, true>(std::forward<Args>(args)...);
    } else if (id == Integer16BitsColorDepthID) {
        return writeLayerWithSwap<KoBgrU16Traits, true>(std::forward<Args>(args)...);
#ifdef HAVE_OPENEXR
    } else if (id == Float16BitsColorDepthID) {
        return writeLayerWithSwap<KoBgrF16Traits, false>(std::forward<Args>(args)...);
#endif
    } else if (id == Float32BitsColorDepthID) {
        return writeLayerWithSwap<KoBgrF32Traits, false>(std::forward<Args>(args)...);
    } else {
        KIS_ASSERT_X(false, "JPEGXLExport::writeLayer", "unsupported bit depth!");
        return QByteArray();
    }
}
} // namespace HDR

JPEGXLExport::JPEGXLExport(QObject *parent, const QVariantList &)
    : KisImportExportFilter(parent)
{
}

KisImportExportErrorCode JPEGXLExport::convert(KisDocument *document, QIODevice *io, KisPropertiesConfigurationSP cfg)
{
    KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(io->isWritable(), ImportExportCodes::NoAccessToWrite);

    KisImageSP image = document->savingImage();
    const QRect bounds = image->bounds();

    auto enc = JxlEncoderMake(nullptr);
    auto runner = JxlResizableParallelRunnerMake(nullptr);
    if (JXL_ENC_SUCCESS != JxlEncoderSetParallelRunner(enc.get(), JxlResizableParallelRunner, runner.get())) {
        errFile << "JxlEncoderSetParallelRunner failed";
        return ImportExportCodes::InternalError;
    }

    JxlResizableParallelRunnerSetThreads(runner.get(),
                                         JxlResizableParallelRunnerSuggestThreads(static_cast<uint64_t>(bounds.width()), static_cast<uint64_t>(bounds.height())));

    const KoColorSpace *cs = image->colorSpace();
    ConversionPolicy conversionPolicy = ConversionPolicy::KeepTheSame;
    bool convertToRec2020 = false;

    if (cs->hasHighDynamicRange() && cs->colorModelId() != GrayAColorModelID) {
        const QString conversionOption = (cfg->getString("floatingPointConversionOption", "Rec2100PQ"));
        if (conversionOption == "Rec2100PQ") {
            convertToRec2020 = true;
            conversionPolicy = ConversionPolicy::ApplyPQ;
        } else if (conversionOption == "Rec2100HLG") {
            convertToRec2020 = true;
            conversionPolicy = ConversionPolicy::ApplyHLG;
        } else if (conversionOption == "ApplyPQ") {
            conversionPolicy = ConversionPolicy::ApplyPQ;
        } else if (conversionOption == "ApplyHLG") {
            conversionPolicy = ConversionPolicy::ApplyHLG;
        } else if (conversionOption == "ApplySMPTE428") {
            conversionPolicy = ConversionPolicy::ApplySMPTE428;
        }
    }

    if (cs->hasHighDynamicRange() && convertToRec2020) {
        const KoColorProfile *linear =
            KoColorSpaceRegistry::instance()->profileFor({}, PRIMARIES_ITU_R_BT_2020_2_AND_2100_0, TRC_LINEAR);
        KIS_ASSERT_RECOVER(linear)
        {
            errFile << "Unable to find a working profile for Rec. 2020";
            return ImportExportCodes::FormatColorSpaceUnsupported;
        }
        const KoColorSpace *linearRec2020 =
            KoColorSpaceRegistry::instance()->colorSpace(RGBAColorModelID.id(), Float32BitsColorDepthID.id(), linear);
        image->convertImageColorSpace(linearRec2020,
                                      KoColorConversionTransformation::internalRenderingIntent(),
                                      KoColorConversionTransformation::internalConversionFlags());

        image->waitForDone();
        cs = image->colorSpace();
    }

    const float hlgGamma = cfg->getFloat("HLGgamma", 1.2f);
    const float hlgNominalPeak = cfg->getFloat("HLGnominalPeak", 1000.0f);
    const bool removeHGLOOTF = cfg->getBool("removeHGLOOTF", true);

    const JxlPixelFormat pixelFormat = [&]() {
        JxlPixelFormat pixelFormat{};
        if (cs->colorDepthId() == Integer8BitsColorDepthID) {
            pixelFormat.data_type = JXL_TYPE_UINT8;
        } else if (conversionPolicy != ConversionPolicy::KeepTheSame
                   || cs->colorDepthId() == Integer16BitsColorDepthID) {
            pixelFormat.data_type = JXL_TYPE_UINT16;
#ifdef HAVE_OPENEXR
        } else if (cs->colorDepthId() == Float16BitsColorDepthID) {
            pixelFormat.data_type = JXL_TYPE_FLOAT16;
#endif
        } else if (cs->colorDepthId() == Float32BitsColorDepthID) {
            pixelFormat.data_type = JXL_TYPE_FLOAT;
        }
        if (cs->colorModelId() == RGBAColorModelID) {
            pixelFormat.num_channels = 4;
        } else if (cs->colorModelId() == GrayAColorModelID) {
            pixelFormat.num_channels = 2;
        }
        return pixelFormat;
    }();

    if (JXL_ENC_SUCCESS != JxlEncoderUseBoxes(enc.get())) {
        errFile << "JxlEncoderUseBoxes failed";
        return ImportExportCodes::InternalError;
    }

    const auto basicInfo = [&]() {
        auto info{std::make_unique<JxlBasicInfo>()};
        JxlEncoderInitBasicInfo(info.get());
        info->xsize = static_cast<uint32_t>(bounds.width());
        info->ysize = static_cast<uint32_t>(bounds.height());
        {
            if (pixelFormat.data_type == JXL_TYPE_UINT8) {
                info->bits_per_sample = 8;
                info->exponent_bits_per_sample = 0;
                info->alpha_bits = 8;
                info->alpha_exponent_bits = 0;
            } else if (pixelFormat.data_type == JXL_TYPE_UINT16) {
                info->bits_per_sample = 16;
                info->exponent_bits_per_sample = 0;
                info->alpha_bits = 16;
                info->alpha_exponent_bits = 0;
#ifdef HAVE_OPENEXR
            } else if (pixelFormat.data_type == JXL_TYPE_FLOAT16) {
                info->bits_per_sample = 16;
                info->exponent_bits_per_sample = 5;
                info->alpha_bits = 16;
                info->alpha_exponent_bits = 5;
#endif
            } else if (pixelFormat.data_type == JXL_TYPE_FLOAT) {
                info->bits_per_sample = 32;
                info->exponent_bits_per_sample = 8;
                info->alpha_bits = 32;
                info->alpha_exponent_bits = 8;
            }
        }
        if (cs->colorModelId() == RGBAColorModelID) {
            info->num_color_channels = 3;
            info->num_extra_channels = 1;
        } else if (cs->colorModelId() == GrayAColorModelID) {
            info->num_color_channels = 1;
            info->num_extra_channels = 1;
        }
        info->uses_original_profile = JXL_TRUE;
        if (image->animationInterface()->hasAnimation() && cfg->getBool("haveAnimation", true)) {
            info->have_animation = JXL_TRUE;
            info->animation.have_timecodes = JXL_FALSE;
            info->animation.num_loops = 0;
            // Unlike WebP, JXL does allow for setting proper frame rates.
            info->animation.tps_numerator =
                static_cast<uint32_t>(image->animationInterface()->framerate());
            info->animation.tps_denominator = 1;
        }
        return info;
    }();

    if (JXL_ENC_SUCCESS != JxlEncoderSetBasicInfo(enc.get(), basicInfo.get())) {
        errFile << "JxlEncoderSetBasicInfo failed";
        return ImportExportCodes::InternalError;
    }

    {
        JxlColorEncoding cicpDescription{};

        switch (conversionPolicy) {
        case ConversionPolicy::ApplyPQ:
            cicpDescription.transfer_function = JXL_TRANSFER_FUNCTION_PQ;
            break;
        case ConversionPolicy::ApplyHLG:
            cicpDescription.transfer_function = JXL_TRANSFER_FUNCTION_HLG;
            break;
        case ConversionPolicy::ApplySMPTE428:
            cicpDescription.transfer_function = JXL_TRANSFER_FUNCTION_DCI;
            break;
        case ConversionPolicy::KeepTheSame:
        default: {
            const TransferCharacteristics gamma = cs->profile()->getTransferCharacteristics();
            switch (gamma) {
            case TRC_LINEAR:
                cicpDescription.transfer_function = JXL_TRANSFER_FUNCTION_LINEAR;
                break;
            case TRC_ITU_R_BT_709_5:
            case TRC_ITU_R_BT_601_6:
            case TRC_ITU_R_BT_2020_2_10bit:
                cicpDescription.transfer_function = JXL_TRANSFER_FUNCTION_709;
                break;
            case TRC_ITU_R_BT_470_6_SYSTEM_M:
                cicpDescription.transfer_function = JXL_TRANSFER_FUNCTION_GAMMA;
                cicpDescription.gamma = 2.2;
                break;
            case TRC_ITU_R_BT_470_6_SYSTEM_B_G:
                cicpDescription.transfer_function = JXL_TRANSFER_FUNCTION_GAMMA;
                cicpDescription.gamma = 2.8;
                break;
            case TRC_IEC_61966_2_1:
                cicpDescription.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
                break;
            case TRC_ITU_R_BT_2100_0_PQ:
                cicpDescription.transfer_function = JXL_TRANSFER_FUNCTION_PQ;
                break;
            case TRC_SMPTE_ST_428_1:
                cicpDescription.transfer_function = JXL_TRANSFER_FUNCTION_DCI;
                break;
            case TRC_ITU_R_BT_2100_0_HLG:
                cicpDescription.transfer_function = JXL_TRANSFER_FUNCTION_HLG;
                break;
            case TRC_GAMMA_1_8:
                cicpDescription.transfer_function = JXL_TRANSFER_FUNCTION_GAMMA;
                cicpDescription.gamma = 1.8;
                break;
            case TRC_GAMMA_2_4:
                cicpDescription.transfer_function = JXL_TRANSFER_FUNCTION_GAMMA;
                cicpDescription.gamma = 2.4;
                break;
            case TRC_PROPHOTO:
                cicpDescription.transfer_function = JXL_TRANSFER_FUNCTION_GAMMA;
                cicpDescription.gamma = 1.8;
                break;
            case TRC_A98:
                cicpDescription.transfer_function = JXL_TRANSFER_FUNCTION_GAMMA;
                cicpDescription.gamma = 256.0 / 563.0;
                break;
            case TRC_ITU_R_BT_2020_2_12bit:
            case TRC_ITU_R_BT_1361:
            case TRC_SMPTE_240M:
            case TRC_LOGARITHMIC_100:
            case TRC_LOGARITHMIC_100_sqrt10:
            case TRC_IEC_61966_2_4:
            case TRC_UNSPECIFIED:
                if (cs->profile()->isLinear()) {
                    cicpDescription.transfer_function = JXL_TRANSFER_FUNCTION_LINEAR;
                } else {
                    dbgFile << "JXL CICP cannot describe the current transfer function" << gamma
                            << ", falling back to ICC";
                    cicpDescription.transfer_function = JXL_TRANSFER_FUNCTION_UNKNOWN;
                }
                break;
            }
        } break;
        }

        const ColorPrimaries primaries = cs->profile()->getColorPrimaries();

        const bool arePrimariesSupported =
            (primaries == PRIMARIES_ITU_R_BT_709_5 || primaries == PRIMARIES_ITU_R_BT_2020_2_AND_2100_0 || primaries == PRIMARIES_SMPTE_RP_431_2);

        if (cicpDescription.transfer_function == JXL_TRANSFER_FUNCTION_UNKNOWN || !arePrimariesSupported) {
            const QByteArray profile = cs->profile()->rawData();

            if (JXL_ENC_SUCCESS
                != JxlEncoderSetICCProfile(enc.get(), reinterpret_cast<const uint8_t *>(profile.constData()), static_cast<size_t>(profile.size()))) {
                errFile << "JxlEncoderSetICCProfile failed";
                return ImportExportCodes::InternalError;
            }
        } else {
            switch (primaries) {
            case PRIMARIES_ITU_R_BT_709_5:
                cicpDescription.primaries = JXL_PRIMARIES_SRGB;
                break;
            case PRIMARIES_ITU_R_BT_2020_2_AND_2100_0:
                cicpDescription.primaries = JXL_PRIMARIES_2100;
                break;
            case PRIMARIES_SMPTE_RP_431_2:
                cicpDescription.primaries = JXL_PRIMARIES_P3;
                break;
            default:
                KIS_SAFE_ASSERT_RECOVER_NOOP(false && "Writing possibly non-roundtrip primaries!");
                const QVector<qreal> colorants = cs->profile()->getColorantsxyY();
                cicpDescription.primaries = JXL_PRIMARIES_CUSTOM;
                cicpDescription.primaries_red_xy[0] = colorants[0];
                cicpDescription.primaries_red_xy[1] = colorants[1];
                cicpDescription.primaries_green_xy[0] = colorants[3];
                cicpDescription.primaries_green_xy[1] = colorants[4];
                cicpDescription.primaries_blue_xy[0] = colorants[6];
                cicpDescription.primaries_blue_xy[1] = colorants[7];
                break;
            }

            // Unfortunately, Wolthera never wrote an enum for white points...
            const QVector<qreal> whitePoint = image->colorSpace()->profile()->getWhitePointxyY();
            cicpDescription.white_point = JXL_WHITE_POINT_CUSTOM;
            cicpDescription.white_point_xy[0] = whitePoint[0];
            cicpDescription.white_point_xy[1] = whitePoint[1];

            if (JXL_ENC_SUCCESS != JxlEncoderSetColorEncoding(enc.get(), &cicpDescription)) {
                errFile << "JxlEncoderSetColorEncoding failed";
                return ImportExportCodes::InternalError;
            }
        }
    }

    if (cfg->getBool("storeMetadata", false)) {
        auto metaDataStore = [&]() -> std::unique_ptr<KisMetaData::Store> {
            KisExifInfoVisitor exivInfoVisitor;
            exivInfoVisitor.visit(image->rootLayer().data());
            if (exivInfoVisitor.metaDataCount() == 1) {
                return std::make_unique<KisMetaData::Store>(*exivInfoVisitor.exifInfo());
            } else {
                return {};
            }
        }();

        if (metaDataStore && !metaDataStore->isEmpty()) {
            KisMetaData::FilterRegistryModel model;
            model.setEnabledFilters(cfg->getString("filters").split(","));
            metaDataStore->applyFilters(model.enabledFilters());
        }

        if (metaDataStore && cfg->getBool("exif", true)) {
            const KisMetaData::IOBackend *io = KisMetadataBackendRegistry::instance()->value("exif");

            QBuffer ioDevice;

            // Inject the data as any other IOBackend
            io->saveTo(metaDataStore.get(), &ioDevice);

            if (JXL_ENC_SUCCESS
                != JxlEncoderAddBox(enc.get(),
                                    "Exif",
                                    reinterpret_cast<const uint8_t *>(ioDevice.data().constData()),
                                    static_cast<size_t>(ioDevice.size()),
                                    JXL_FALSE)) {
                errFile << "JxlEncoderAddBox for EXIF failed";
                return ImportExportCodes::InternalError;
            }
        }

        if (metaDataStore && cfg->getBool("xmp", true)) {
            const KisMetaData::IOBackend *io = KisMetadataBackendRegistry::instance()->value("xmp");

            QBuffer ioDevice;

            // Inject the data as any other IOBackend
            io->saveTo(metaDataStore.get(), &ioDevice);

            if (JXL_ENC_SUCCESS
                != JxlEncoderAddBox(enc.get(),
                                    "xml ",
                                    reinterpret_cast<const uint8_t *>(ioDevice.data().constData()),
                                    static_cast<size_t>(ioDevice.size()),
                                    JXL_FALSE)) {
                errFile << "JxlEncoderAddBox for XMP failed";
                return ImportExportCodes::InternalError;
            }
        }

        if (metaDataStore && cfg->getBool("iptc", true)) {
            const KisMetaData::IOBackend *io = KisMetadataBackendRegistry::instance()->value("iptc");

            QBuffer ioDevice;

            // Inject the data as any other IOBackend
            io->saveTo(metaDataStore.get(), &ioDevice);

            if (JXL_ENC_SUCCESS
                != JxlEncoderAddBox(enc.get(),
                                    "xml ",
                                    reinterpret_cast<const uint8_t *>(ioDevice.data().constData()),
                                    static_cast<size_t>(ioDevice.size()),
                                    JXL_FALSE)) {
                errFile << "JxlEncoderAddBox for IPTC failed";
                return ImportExportCodes::InternalError;
            }
        }
    }

    auto *frameSettings = JxlEncoderFrameSettingsCreate(enc.get(), nullptr);
    {
        const auto setFrameLossless = [&](bool v) {
            if (JxlEncoderSetFrameLossless(frameSettings, v ? JXL_TRUE : JXL_FALSE) != JXL_ENC_SUCCESS) {
                errFile << "JxlEncoderSetFrameLossless failed";
                return false;
            }
            return true;
        };

        const auto setSetting = [&](JxlEncoderFrameSettingId id, int v) {
            // https://github.com/libjxl/libjxl/issues/1210
            if (id == JXL_ENC_FRAME_SETTING_RESAMPLING && v == -1)
                return true;
            if (JxlEncoderFrameSettingsSetOption(frameSettings, id, v) != JXL_ENC_SUCCESS) {
                errFile << "JxlEncoderFrameSettingsSetOption failed";
                return false;
            }
            return true;
        };

        // Hardcoded a map function that translates from arbitrary quality value to JPEG-XL distance
        const auto setDistance = [&](float v) {
            // Using a gamma curve to map the quality -> distance a bit better
            float y = pow(v / 100.0f, 1.0f / 2.2f) * 100.0f;
            float dist = cfg->getBool("lossless") ? 0.0f : ((y * (1.0f - 25.0f)) / 100.0f) + 25.0f;
            dbgFile << "libjxl distance equivalent: " << dist;
            return JxlEncoderSetFrameDistance(frameSettings, dist) == JXL_ENC_SUCCESS;
        };

        if (!setFrameLossless(cfg->getBool("lossless"))
            || !setSetting(JXL_ENC_FRAME_SETTING_EFFORT, cfg->getInt("effort", 7))
            || !setSetting(JXL_ENC_FRAME_SETTING_DECODING_SPEED, cfg->getInt("decodingSpeed", 0))
            || !setSetting(JXL_ENC_FRAME_SETTING_RESAMPLING, cfg->getInt("resampling", -1))
            || !setSetting(JXL_ENC_FRAME_SETTING_EXTRA_CHANNEL_RESAMPLING, cfg->getInt("extraChannelResampling", -1))
            || !setSetting(JXL_ENC_FRAME_SETTING_DOTS, cfg->getInt("dots", -1))
            || !setSetting(JXL_ENC_FRAME_SETTING_PATCHES, cfg->getInt("patches", -1))
            || !setSetting(JXL_ENC_FRAME_SETTING_EPF, cfg->getInt("epf", -1))
            || !setSetting(JXL_ENC_FRAME_SETTING_GABORISH, cfg->getInt("gaborish", -1))
            || !setSetting(JXL_ENC_FRAME_SETTING_MODULAR, cfg->getInt("modular", -1))
            || !setSetting(JXL_ENC_FRAME_SETTING_KEEP_INVISIBLE, cfg->getInt("keepInvisible", -1))
            || !setSetting(JXL_ENC_FRAME_SETTING_GROUP_ORDER, cfg->getInt("groupOrder", -1))
            || !setSetting(JXL_ENC_FRAME_SETTING_RESPONSIVE, cfg->getInt("responsive", -1))
            || !setSetting(JXL_ENC_FRAME_SETTING_PROGRESSIVE_AC, cfg->getInt("progressiveAC", -1))
            || !setSetting(JXL_ENC_FRAME_SETTING_QPROGRESSIVE_AC, cfg->getInt("qProgressiveAC", -1))
            || !setSetting(JXL_ENC_FRAME_SETTING_PROGRESSIVE_DC, cfg->getInt("progressiveDC", -1))
            || !setSetting(JXL_ENC_FRAME_SETTING_PALETTE_COLORS, cfg->getInt("paletteColors", -1))
            || !setSetting(JXL_ENC_FRAME_SETTING_LOSSY_PALETTE, cfg->getInt("lossyPalette", -1))
            || !setSetting(JXL_ENC_FRAME_SETTING_MODULAR_GROUP_SIZE, cfg->getInt("modularGroupSize", -1))
            || !setSetting(JXL_ENC_FRAME_SETTING_MODULAR_PREDICTOR, cfg->getInt("modularPredictor", -1))
            || !setSetting(JXL_ENC_FRAME_SETTING_JPEG_RECON_CFL, cfg->getInt("jpegReconCFL", -1))
            || !setDistance(cfg->getInt("lossyQuality", 100))) {
            return ImportExportCodes::InternalError;
        }
    }

    {
        const auto setSettingFloat = [&](JxlEncoderFrameSettingId id, float v) {
            if (JxlEncoderFrameSettingsSetFloatOption(frameSettings, id, v) != JXL_ENC_SUCCESS) {
                errFile << "JxlEncoderFrameSettingsSetFloatOption failed";
                return false;
            }
            return true;
        };

        if (!setSettingFloat(JXL_ENC_FRAME_SETTING_PHOTON_NOISE, cfg->getFloat("photonNoise", 0))
            || !setSettingFloat(JXL_ENC_FRAME_SETTING_CHANNEL_COLORS_GLOBAL_PERCENT,
                                cfg->getFloat("channelColorsGlobalPercent", -1))
            || !setSettingFloat(JXL_ENC_FRAME_SETTING_CHANNEL_COLORS_GROUP_PERCENT,
                                cfg->getFloat("channelColorsGroupPercent", -1))
            || !setSettingFloat(JXL_ENC_FRAME_SETTING_MODULAR_MA_TREE_LEARNING_PERCENT,
                                cfg->getFloat("modularMATreeLearningPercent", -1))) {
            return ImportExportCodes::InternalError;
        }
    }

    {
        if (image->animationInterface()->hasAnimation()
            && cfg->getBool("haveAnimation", true)) {
            // Flatten the image, projections don't have keyframes.
            KisLayerUtils::flattenImage(image, nullptr);
            image->waitForDone();

            const KisNodeSP projection = image->rootLayer()->firstChild();
            KIS_ASSERT(projection->isAnimated());
            KIS_ASSERT(projection->hasEditablePaintDevice());

            const auto *frames = projection->paintDevice()->keyframeChannel();
            const auto times = [&]() {
                auto t = frames->allKeyframeTimes().toList();
                std::sort(t.begin(), t.end());
                return t;
            }();

            auto frameHeader = []() {
                auto header = std::make_unique<JxlFrameHeader>();
                JxlEncoderInitFrameHeader(header.get());
                return header;
            }();

            for (const auto i : times) {
                frameHeader->duration = [&]() {
                    const auto nextKeyframe = frames->nextKeyframeTime(i);
                    if (nextKeyframe == -1) {
                        return static_cast<uint32_t>(
                            image->animationInterface()->fullClipRange().end()
                            - i + 1);
                    } else {
                        return static_cast<uint32_t>(frames->nextKeyframeTime(i) - i);
                    }
                }();
                frameHeader->is_last = 0;

                if (JxlEncoderSetFrameHeader(frameSettings, frameHeader.get()) != JXL_ENC_SUCCESS) {
                    errFile << "JxlEncoderSetFrameHeader failed";
                    return ImportExportCodes::InternalError;
                }

                const QByteArray pixels = [&]() {
                    const auto frameData = frames->keyframeAt<KisRasterKeyframe>(i);
                    KisPaintDeviceSP dev =
                        new KisPaintDevice(*image->projection(), KritaUtils::DeviceCopyMode::CopySnapshot);
                    frameData->writeFrameToDevice(dev);

                    const KoID colorModel = cs->colorModelId();

                    if (colorModel != RGBAColorModelID) {
                        // blast it wholesale
                        QByteArray p;
                        p.resize(bounds.width() * bounds.height() * static_cast<int>(cs->pixelSize()));
                        dev->readBytes(reinterpret_cast<quint8 *>(p.data()), bounds);
                        return p;
                    } else {
                        KisHLineConstIteratorSP it = dev->createHLineConstIteratorNG(0, 0, bounds.width());

                        // detect traits based on depth
                        // if u8 or u16, also trigger swap
                        return HDR::writeLayer(cs->colorDepthId(),
                                               convertToRec2020,
                                               cs->profile()->isLinear(),
                                               conversionPolicy,
                                               removeHGLOOTF,
                                               bounds.width(),
                                               bounds.height(),
                                               it,
                                               hlgGamma,
                                               hlgNominalPeak,
                                               cs);
                    }
                }();

                if (JxlEncoderAddImageFrame(frameSettings,
                                            &pixelFormat,
                                            pixels.data(),
                                            static_cast<size_t>(pixels.size()))
                    != JXL_ENC_SUCCESS) {
                    errFile << "JxlEncoderAddImageFrame @" << i << "failed";
                    return ImportExportCodes::InternalError;
                }
            }
        } else {
            // Insert the projection itself only
            const QByteArray pixels = [&]() {
                const KisPaintDeviceSP dev = image->projection();

                const KoID colorModel = cs->colorModelId();
                const KoID colorDepth = cs->colorDepthId();

                if (colorModel != RGBAColorModelID
                    || (colorDepth != Integer8BitsColorDepthID && colorDepth != Integer16BitsColorDepthID
                        && conversionPolicy == ConversionPolicy::KeepTheSame)) {
                    // blast it wholesale
                    QByteArray p;
                    p.resize(bounds.width() * bounds.height() * static_cast<int>(cs->pixelSize()));
                    dev->readBytes(reinterpret_cast<quint8 *>(p.data()), bounds);
                    return p;
                } else {
                    KisHLineConstIteratorSP it = dev->createHLineConstIteratorNG(0, 0, image->width());

                    // detect traits based on depth
                    // if u8 or u16, also trigger swap
                    return HDR::writeLayer(cs->colorDepthId(),
                                           convertToRec2020,
                                           cs->profile()->isLinear(),
                                           conversionPolicy,
                                           removeHGLOOTF,
                                           image->width(),
                                           image->height(),
                                           it,
                                           hlgGamma,
                                           hlgNominalPeak,
                                           cs);
                }
            }();

            if (JxlEncoderAddImageFrame(frameSettings, &pixelFormat, pixels.data(), static_cast<size_t>(pixels.size()))
                != JXL_ENC_SUCCESS) {
                errFile << "JxlEncoderAddImageFrame failed";
                return ImportExportCodes::InternalError;
            }
        }
        JxlEncoderCloseInput(enc.get());

        QByteArray compressed(16384, 0x0);
        auto *nextOut = reinterpret_cast<uint8_t *>(compressed.data());
        auto availOut = static_cast<size_t>(compressed.size());
        auto result = JXL_ENC_NEED_MORE_OUTPUT;
        while (result == JXL_ENC_NEED_MORE_OUTPUT) {
            result = JxlEncoderProcessOutput(enc.get(), &nextOut, &availOut);
            if (result != JXL_ENC_ERROR) {
                io->write(compressed.data(), compressed.size() - static_cast<int>(availOut));
            }
            if (result == JXL_ENC_NEED_MORE_OUTPUT) {
                compressed.resize(compressed.size() * 2);
                nextOut = reinterpret_cast<uint8_t *>(compressed.data());
                availOut = static_cast<size_t>(compressed.size());
            }
        }
        if (JXL_ENC_SUCCESS != result) {
            errFile << "JxlEncoderProcessOutput failed";
            return ImportExportCodes::ErrorWhileWriting;
        }
    }

    return ImportExportCodes::OK;
}

void JPEGXLExport::initializeCapabilities()
{
    // This checks before saving for what the file format supports: anything that is supported needs to be mentioned
    // here

    QList<QPair<KoID, KoID>> supportedColorModels;
    addCapability(KisExportCheckRegistry::instance()
                      ->get("AnimationCheck")
                      ->create(KisExportCheckBase::SUPPORTED));
    addCapability(KisExportCheckRegistry::instance()->get("sRGBProfileCheck")->create(KisExportCheckBase::SUPPORTED));
    addCapability(KisExportCheckRegistry::instance()->get("ExifCheck")->create(KisExportCheckBase::SUPPORTED));
    addCapability(KisExportCheckRegistry::instance()->get("MultiLayerCheck")->create(KisExportCheckBase::PARTIALLY));
    addCapability(KisExportCheckRegistry::instance()->get("TiffExifCheck")->create(KisExportCheckBase::PARTIALLY));
    supportedColorModels << QPair<KoID, KoID>() << QPair<KoID, KoID>(RGBAColorModelID, Integer8BitsColorDepthID)
                         << QPair<KoID, KoID>(GrayAColorModelID, Integer8BitsColorDepthID)
                         << QPair<KoID, KoID>(RGBAColorModelID, Integer16BitsColorDepthID)
                         << QPair<KoID, KoID>(GrayAColorModelID, Integer16BitsColorDepthID)
#ifdef HAVE_OPENEXR
                         << QPair<KoID, KoID>(RGBAColorModelID, Float16BitsColorDepthID)
                         << QPair<KoID, KoID>(GrayAColorModelID, Float16BitsColorDepthID)
#endif
                         << QPair<KoID, KoID>(RGBAColorModelID, Float32BitsColorDepthID)
                         << QPair<KoID, KoID>(GrayAColorModelID, Float32BitsColorDepthID);
    addSupportedColorModels(supportedColorModels, "JPEG-XL");
}

KisConfigWidget *
JPEGXLExport::createConfigurationWidget(QWidget *parent, const QByteArray & /*from*/, const QByteArray & /*to*/) const
{
    return new KisWdgOptionsJPEGXL(parent);
}

KisPropertiesConfigurationSP JPEGXLExport::defaultConfiguration(const QByteArray &, const QByteArray &) const
{
    KisPropertiesConfigurationSP cfg = new KisPropertiesConfiguration();

    // WARNING: libjxl only allows setting encoding properties,
    // so I hardcoded values from https://libjxl.readthedocs.io/en/latest/api_encoder.html
    // https://readthedocs.org/projects/libjxl/builds/16271112/

    // Options for the following were not added because they rely
    // on the image's specific color space, or can introduce more
    // trouble than help:
    //   JXL_ENC_FRAME_SETTING_GROUP_ORDER_CENTER_X
    //   JXL_ENC_FRAME_SETTING_GROUP_ORDER_CENTER_Y
    //   JXL_ENC_FRAME_SETTING_MODULAR_COLOR_SPACE
    //   JXL_ENC_FRAME_SETTING_MODULAR_NB_PREV_CHANNELS
    // These are directly incompatible with the export logic:
    //   JXL_ENC_FRAME_SETTING_ALREADY_DOWNSAMPLED
    //   JXL_ENC_FRAME_SETTING_COLOR_TRANSFORM

    cfg->setProperty("haveAnimation", true);
    cfg->setProperty("lossless", true);
    cfg->setProperty("effort", 7);
    cfg->setProperty("decodingSpeed", 0);
    cfg->setProperty("lossyQuality", 100);
    cfg->setProperty("forceModular", false);
    cfg->setProperty("modularSetVal", -1);

    cfg->setProperty("floatingPointConversionOption", "KeepSame");
    cfg->setProperty("HLGnominalPeak", 1000.0);
    cfg->setProperty("HLGgamma", 1.2);
    cfg->setProperty("removeHGLOOTF", true);

    cfg->setProperty("resampling", -1);
    cfg->setProperty("extraChannelResampling", -1);
    cfg->setProperty("photonNoise", 0);
    cfg->setProperty("dots", -1);
    cfg->setProperty("patches", -1);
    cfg->setProperty("epf", -1);
    cfg->setProperty("gaborish", -1);
    cfg->setProperty("modular", -1);
    cfg->setProperty("keepInvisible", -1);
    cfg->setProperty("groupOrder", -1);
    cfg->setProperty("responsive", -1);
    cfg->setProperty("progressiveAC", -1);
    cfg->setProperty("qProgressiveAC", -1);
    cfg->setProperty("progressiveDC", -1);
    cfg->setProperty("channelColorsGlobalPercent", -1);
    cfg->setProperty("channelColorsGroupPercent", -1);
    cfg->setProperty("paletteColors", -1);
    cfg->setProperty("lossyPalette", -1);
    cfg->setProperty("modularGroupSize", -1);
    cfg->setProperty("modularPredictor", -1);
    cfg->setProperty("modularMATreeLearningPercent", -1);
    cfg->setProperty("jpegReconCFL", -1);

    cfg->setProperty("exif", true);
    cfg->setProperty("xmp", true);
    cfg->setProperty("iptc", true);
    cfg->setProperty("storeMetadata", false);
    cfg->setProperty("filters", "");
    return cfg;
}

#include <JPEGXLExport.moc>
