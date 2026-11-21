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
//   diffusion model  reads T5 when it has vector_in (FLUX.1), SD3's context_embedder, or is Wan
//                    (text_embedding beside patch_embedding), else an LLM (Z-Image, FLUX.2,
//                    Qwen-Image); wants a video VAE when it is Qwen-Image (time_text_embed beside
//                    txt_in) or Wan
//   VAE              a video VAE when its first decoder convolution is 3D (Qwen-Image, Wan)
//
// A LoRA fits a model whose layers it changes are there with the same input widths (loraFits).
//
// A LoRA that changes a layer of a block family the model has, at an index it does not (FLUX.1's
// double_blocks.18 on Klein 4B, as wide but shallower), does not fit either.
//
// A LoRA none of whose layers is found in the model, under any naming, does not fit when it is
// for a UNet and the model is not (or the other way round), or when most of its layers take a
// width the model's blocks do not (a 3072-wide FLUX.1 LoRA on the 4096-wide Klein 9B).
//
// Families and formats, from tensor names too: an LLM encoder is Qwen3 (q_norm in its
// attention), Qwen3.5 (linear_attn layers) or Qwen2.5 (neither); Z-Image and FLUX.2 read Qwen3,
// Qwen-Image Qwen2.5-VL, so another family does not fit. A file in a quantization
// stable-diffusion.cpp cannot load fits nothing: ComfyUI's mixed quantization (comfy_quant
// tensors: int8 ConvRot, fp8mixed; the helper aborts on them), GPTQ (weight_codebook), SVDQuant
// (Nunchaku: qweight with proj_down), AWQ (qweight with scaled_zeros).
//
// What a file is (ModelShape::kind): a LoRA by its low-rank tensors (lora_A/lora_B, lora_up/
// lora_down, LoKr, LoHa), a checkpoint by a diffusion model and a VAE decoder in one file, else
// a diffusion model, a VAE or a text encoder by the figures above.
//
// A text encoder fits a diffusion model of its kind whose text width is 1 to 3 times its hidden
// size (FLUX.2 Klein reads three Qwen3 layers stacked: 7680 = 3 x 2560); a VAE fits one of its
// kind whose latent channels are its own. A figure the header does not give leaves the fit
// unknown, never "does not fit".

#include <QHash>
#include <QMutex>
#include <QSet>
#include <QString>

#include <optional>

namespace fugue {

enum class TextKind { Llm, T5, Clip };

// What a file is, by its tensor names (#86): for a folder of one kind of model.
enum class ModelKind { DiffusionModel, TextEncoder, Vae, Lora, Checkpoint };

struct ModelShape {
    std::optional<int> textWidth;      // a diffusion model's
    std::optional<int> latentChannels; // a diffusion model's or a VAE's
    std::optional<int> hiddenSize;     // a text encoder's
    // A text encoder's kind, or the kind a diffusion model reads.
    std::optional<TextKind> textKind;
    // A VAE's: whether it is a video VAE; a diffusion model's: whether it wants one.
    std::optional<bool> videoVae;
    std::optional<ModelKind> kind;
    // A diffusion model's linear and convolution layers, a LoRA's changed ones (up to
    // kLoraLayersKept, spread over its names): the widths of each, by layerKey().
    struct Widths {
        int in = 0;
        int out = 0;
        bool operator==(const Widths &other) const { return in == other.in && out == other.out; }
    };
    QHash<QString, Widths> layers;
    // A diffusion model's layer keys with every number "#" (double.blocks.#.img.attn.proj): a
    // LoRA's layer of such a family at an index the model lacks is for a deeper model.
    QSet<QString> layerFamilies;
    // A LoRA named as ComfyUI saves one (diffusion_model.<layer>), which stable-diffusion.cpp
    // applies as it is: none of its layers in a model means it is for another model.
    bool nativeLoraNames = false;
    // Whether a diffusion model is a UNet (SD 1.5, SDXL: input_blocks, down_blocks) or a LoRA
    // changes one; nothing when not told.
    std::optional<bool> unet;
    // A transformer diffusion model's block width (its image projection's output), or the width
    // most of a LoRA's layers take; nothing when not told.
    std::optional<int> hidden;
    // A language model text encoder's family ("qwen3", "qwen3.5", "qwen2.5"), or the one a
    // diffusion model reads; empty when not known.
    QString llmFamily;
    // The quantization stable-diffusion.cpp has no kernels for that the file is in ("ComfyUI
    // mixed quantization", "GPTQ"); empty when it can load it.
    QString unloadable;
};

// What a file is, as a pack.json resource type names the files it holds ("files", 6.4):
// "diffusion-model", "text-encoder", "vae", "lora" or "checkpoint".
QString modelKindName(ModelKind kind);

// How many of a LoRA's layers a shape keeps: enough to see its blocks and widths, few enough
// for a folder of hundreds.
inline constexpr int kLoraLayersKept = 64;

// A layer's name as both a model and a LoRA write it: without the usual prefixes
// (model.diffusion_model., diffusion_model., transformer., lora_unet_, ...) and the tensor's
// suffix (.weight, .lora_A.weight, ...), every run of "." and "_" one ".", so kohya's
// lora_unet_double_blocks_0_x and double_blocks.0.x meet, and blocks.1.10 is not blocks.11.0.
QString layerKey(const QString &tensorName);
// layerKey() with every number "#": the family a layer belongs to.
QString layerFamily(const QString &key);

// The shape of the file at `path`; nothing when it is neither GGUF nor safetensors or cannot be
// read. Reads only the header (megabytes at most, for a tokenizer held in GGUF metadata).
std::optional<ModelShape> readModelShape(const QString &path);

enum class Fit { Unknown, Fits, DoesNotFit };

Fit textEncoderFits(const ModelShape &diffusionModel, const ModelShape &textEncoder);
// textEncoderFits for Load Models, which loads one LLM encoder (`llm_path`, spec 14.3), or Wan's
// UMT5 (`t5xxl_path`): another T5 or a CLIP encoder does not fit there, whatever its width (FLUX.1
// and SD3 come later).
Fit loadableTextEncoderFits(const ModelShape &diffusionModel, const ModelShape &textEncoder);
Fit vaeFits(const ModelShape &diffusionModel, const ModelShape &vae);
// A LoRA fits a diffusion model when the layers it changes that the model has are as wide in
// both, and at least one is found; none found tells nothing (another naming, as kohya's SDXL
// names against a UNet's); one of another width does not fit (a Klein 4B LoRA on Klein 9B).
// `mismatch`, when given, gets the first layer that is another width: its key and both widths.
struct LayerMismatch {
    QString layer;
    ModelShape::Widths lora, model;
};
Fit loraFits(const ModelShape &diffusionModel, const ModelShape &lora, LayerMismatch *mismatch = nullptr);

// readModelShape remembered per file, for as long as its size and modification time stay.
// Thread-safe.
class ModelShapes {
public:
    // The process's: every reader of headers shares it, so a file is read once.
    static ModelShapes &shared();

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
