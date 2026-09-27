#include <cassert>
#include <sstream>

#include "XenosRecomp/XenosRecomp/shader_source_preservation.h"

int main()
{
    const std::string common = "#ifndef SHADER_COMMON_H_INCLUDED\n#define SHADER_COMMON_H_INCLUDED\n#endif\n";
    const std::string pixelPreamble = "#if defined(__air__) && !defined(XENOS_RECOMP_PIXEL_SHADER)\n"
        "#define XENOS_RECOMP_PIXEL_SHADER\n#endif\n";
    assert(HasCurrentRecoveredShaderPrefix(pixelPreamble + common + "body", common, true));
    assert(HasCurrentRecoveredShaderPrefix(common + "body", common, false));
    assert(!HasCurrentRecoveredShaderPrefix(common + "body", common, true));
    assert(!HasCurrentRecoveredShaderPrefix(pixelPreamble + common + "body", common, false));
    assert(!HasCurrentRecoveredShaderPrefix(pixelPreamble + common + "body", "stale", true));
    std::istringstream valid("liberty-shader-sources-v1\t2\n"
                             "0000000000000001\tshader/preferred.bin\n"
                             "0000000000000002\tshader/second.bin\n");
    const auto sources = ReadPreferredShaderSources(valid);
    assert(sources.size() == 2);
    assert(!IsPreferredShaderSource(sources, 1, "shader/duplicate.bin"));
    assert(IsPreferredShaderSource(sources, 1, "shader/preferred.bin"));
    assert(IsPreferredShaderSource(sources, 3, "shader/new.bin"));
    assert(IsPreferredShaderSource({}, 1, "shader/duplicate.bin"));
    assert(ShaderCacheSourceName("/capture/stage/shader/rage_shaders/a.bin") ==
           "shader/rage_shaders/a.bin");
    assert(ShaderCacheSourceName("C:\\capture\\shader\\rage_shaders\\a.bin") ==
           "shader/rage_shaders/a.bin");
    std::istringstream recovery("liberty-recovered-sources-v1\t2\n"
        "0000000000000001\tpixel\t1794\t3\tshader/preferred.bin\t1.early.hlsl\t1.late.hlsl\n"
        "0000000000000002\tvertex\t0\t0\tshader/second.bin\t2.early.hlsl\t-\n");
    const auto recovered = ReadRecoveredShaderSources(recovery, sources);
    assert(recovered.size() == 2);
    assert(recovered.at(1).isPixelShader);
    assert(recovered.at(1).specConstantsMask == 1794);
    assert(recovered.at(1).usedTextureMask == 3);
    assert(recovered.at(1).late == "1.late.hlsl");
    assert(!recovered.at(2).isPixelShader && recovered.at(2).late.empty());
    for (const char* invalid : {
        "1\tpixel\t1794\t3\tshader/other.bin\t1.hlsl\t-",
        "3\tpixel\t1794\t3\tshader/preferred.bin\t1.hlsl\t-",
        "1\tgeometry\t0\t0\tshader/preferred.bin\t1.hlsl\t-",
        "1\tvertex\t0\t0\tshader/preferred.bin\t1.hlsl\t2.hlsl",
        "1\tpixel\t4294967296\t0\tshader/preferred.bin\t1.hlsl\t-",
        "1\tpixel\t0\t0\tshader/preferred.bin\t../1.hlsl\t-",
        "1\tpixel\t0\t0\tshader/preferred.bin\t-\t-",
        "1\tpixel\t0\t0\tshader/preferred.bin\t1.hlsl\t1.hlsl",
        "1\tpixel\t0\t0\tshader/preferred.bin\t1.hlsl\t-\textra",
        "1\tpixel\t0\t0\tshader/preferred.bin\t1.hlsl"})
    {
        bool rejected = false;
        try
        {
            std::istringstream input(std::string("liberty-recovered-sources-v1\t1\n") + invalid + "\n");
            ReadRecoveredShaderSources(input, sources);
        }
        catch (const std::runtime_error&) { rejected = true; }
        assert(rejected);
    }
    for (const char* invalid : {
             "", "liberty-shader-sources-v1\t0\n",
             "liberty-shader-sources-v1\t2\n1\tshader/a.bin\n",
             "liberty-shader-sources-v1\t2\n1\tshader/a.bin\n1\tshader/b.bin\n",
             "liberty-shader-sources-v1\t1\ninvalid\tshader/a.bin\n",
             "liberty-shader-sources-v1\t1\n1\t\n",
             "liberty-shader-sources-v1\t1\n1\tshader/a.bin\textra\n"})
    {
        bool rejected = false;
        try
        {
            std::istringstream input(invalid);
            ReadPreferredShaderSources(input);
        }
        catch (const std::runtime_error&)
        {
            rejected = true;
        }
        assert(rejected);
    }
}
