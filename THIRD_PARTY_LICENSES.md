# Third-party licenses

RifeOFX combines code and model files from several independent projects. Their
licenses remain applicable to the corresponding files.

## OpenFX SDK

The official OpenFX headers are provided by the Academy Software Foundation:

https://github.com/AcademySoftwareFoundation/openfx

See the SDK repository for its license and copyright notices.

## RIFE NCNN/Vulkan and NCNN

The inference code is based on:

- https://github.com/nihui/rife-ncnn-vulkan
- https://github.com/Tencent/ncnn

The corresponding source repositories contain their MIT license notices.

## RIFE model weights

The release bundle contains NCNN conversions of the RIFE v4 model weights used
by the following public implementation:

https://github.com/styler00dollar/VapourSynth-RIFE-ncnn-Vulkan

The upstream Practical-RIFE README states that its linked trained model files
are under the same MIT license as that project:

https://github.com/hzwer/Practical-RIFE/blob/main/README.md

The release includes this attribution file and keeps the model directory names
and `flownet.param` / `flownet.bin` format documented. Users and redistributors
must retain these notices and verify that the exact model artifacts they use
remain covered by the applicable upstream terms.
