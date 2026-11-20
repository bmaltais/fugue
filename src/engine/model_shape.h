#pragma once

// How a model file plugs into the others of a model (#85, spec 2.5): read from its header, never
// by hashing or loading it. A GGUF file's metadata and tensor list, or a safetensors file's JSON
// header, give the shapes of a few telling tensors:
//
//   diffusion model  text width     the input of its text projection: cap_embedder.1 (Z-Image,
//                                   Lumina 2), txt_in (FLUX, Qwen-Image), context_embedder (SD3)
//                    latent channels the input of its image projection: x_embedder or img_in, a
//                                   linear one taking 2x2 patches (64 = 16 channels), a
//                                   convolution taking channels (SD3, a UNet's input_blocks.0.0)
//   text encoder     hidden size    GGUF <architecture>.embedding_length, else the width of its
//                                   token embedding
//   VAE              latent channels the input of decoder.conv_in (decoder.conv1 in a video VAE)
//
// Widths alone mix kinds up (T5-XXL and Qwen3-8B are both 4096 wide; FLUX.1's VAE and Qwen-Image's
// both take 16 channels), so the tensor names also tell the kind, never the file's name:
//
//   text encoder     T5 (encoder.block.*), CLIP (text_model.*), else an LLM (GGUF's architecture
//                    says t5 or clip too)
//   diffusion model  reads T5 when it has vector_in (FLUX.1) or SD3's context_embedder, else an
//                    LLM (Z-Image, FLUX.2, Qwen-Image); wants a video VAE when it is Qwen-Image
//                    (time_text_embed beside txt_in)
//   VAE              a video VAE when its first decoder convolution is 3D (Qwen-Image, Wan)
//
// A text encoder fits a diffusion model of its kind whose text width is 1 to 3 times its hidden
// size (FLUX.2 Klein reads three Qwen3 layers stacked: 7680 = 3 x 2560); a VAE fits one of its
// kind whose latent channels are its own. A figure the header does not give leaves the fit
// unknown, never "does not fit".

#include <QHash>
#include <QMutex>
#include <QString>

#include <optional>

namespace fugue {

enum class TextKind { Llm, T5, Clip };

struct ModelShape {
    std::optional<int> textWidth;      // a diffusion model's
    std::optional<int> latentChannels; // a diffusion model's or a VAE's
    std::optional<int> hiddenSize;     // a text encoder's
    // A text encoder's kind, or the kind a diffusion model reads.
    std::optional<TextKind> textKind;
    // A VAE's: whether it is a video VAE; a diffusion model's: whether it wants one.
    std::optional<bool> videoVae;
};

// The shape of the file at `path`; nothing when it is neither GGUF nor safetensors or cannot be
// read. Reads only the header (megabytes at most, for a tokenizer held in GGUF metadata).
std::optional<ModelShape> readModelShape(const QString &path);

enum class Fit { Unknown, Fits, DoesNotFit };

Fit textEncoderFits(const ModelShape &diffusionModel, const ModelShape &textEncoder);
// textEncoderFits for Load Models, which loads one LLM encoder (`llm_path`, spec 14.3): a T5 or
// CLIP encoder does not fit there, whatever its width (FLUX.1 and SD3 come later).
Fit loadableTextEncoderFits(const ModelShape &diffusionModel, const ModelShape &textEncoder);
Fit vaeFits(const ModelShape &diffusionModel, const ModelShape &vae);

// readModelShape remembered per file, for as long as its size and modification time stay.
// Thread-safe.
class ModelShapes {
public:
    std::optional<ModelShape> of(const QString &path);
    // What of() remembers for `path`, without reading it: false when it would have to read.
    bool known(const QString &path, std::optional<ModelShape> *shape);

private:
    struct Entry {
        qint64 size = -1;
        qint64 modified = 0;
        std::optional<ModelShape> shape;
    };
    QMutex m_mutex;
    QHash<QString, Entry> m_entries;
};

} // namespace fugue
