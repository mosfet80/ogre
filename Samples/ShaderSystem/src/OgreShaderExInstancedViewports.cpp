/*
-----------------------------------------------------------------------------
This source file is part of OGRE
(Object-oriented Graphics Rendering Engine)
For the latest info, see http://www.ogre3d.org

Copyright (c) 2000-2014 Torus Knot Software Ltd
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------
*/
#include "OgreShaderExInstancedViewports.h"
#ifdef RTSHADER_SYSTEM_BUILD_EXT_SHADERS
#include "OgreShaderFFPRenderState.h"
#include "OgreShaderProgram.h"
#include "OgreShaderParameter.h"
#include "OgreShaderProgramSet.h"
#include "OgreScriptCompiler.h"
#include "OgreTechnique.h"
#include "OgreHardwareBufferManager.h"
#include "OgreRoot.h"
#include "OgreMaterialManager.h"

namespace Ogre {
namespace RTShader {


/************************************************************************/
/*                                                                      */
/************************************************************************/
String ShaderExInstancedViewports::Type                         = "SGX_InstancedViewports";

//-----------------------------------------------------------------------
ShaderExInstancedViewports::ShaderExInstancedViewports()
{
    mViewportGrid              = Vector2(1.0, 1.0);
    mMonitorsCountChanged       = true;
    mOwnsGlobalData             = false;
    mLayeredTarget              = false;
    mSchemeName                = MSN_SHADERGEN;
}

//-----------------------------------------------------------------------
const String& ShaderExInstancedViewports::getType() const
{
    return Type;
}


//-----------------------------------------------------------------------
int ShaderExInstancedViewports::getExecutionOrder() const
{
    // We place this effect after texturing stage and before fog stage.
    return FFP_POST_PROCESS+1;
}

//-----------------------------------------------------------------------
void ShaderExInstancedViewports::copyFrom(const SubRenderState& rhs)
{
    const ShaderExInstancedViewports& rhsInstancedViewports = static_cast<const ShaderExInstancedViewports&>(rhs);
    
    // Copy all settings that affect this sub render state output code.
    mViewportGrid = rhsInstancedViewports.mViewportGrid;
    mMonitorsCountChanged = rhsInstancedViewports.mMonitorsCountChanged;
    mLayeredTarget = rhsInstancedViewports.mLayeredTarget;
    mSchemeName = rhsInstancedViewports.mSchemeName;
}

//-----------------------------------------------------------------------
bool ShaderExInstancedViewports::preAddToRenderState( const RenderState* renderState, Pass* srcPass, Pass* dstPass )
{
    auto matname = srcPass->getParent()->getParent()->getName();
    return matname.find("SdkTrays") == String::npos && matname.find("Instancing") == String::npos && matname.find("ImGui") == String::npos;
}
//-----------------------------------------------------------------------

bool ShaderExInstancedViewports::createCpuSubPrograms(ProgramSet* programSet)
{
    Program* vsProgram = programSet->getCpuProgram(GPT_VERTEX_PROGRAM);
    Program* psProgram = programSet->getCpuProgram(GPT_FRAGMENT_PROGRAM);

    bool isHLSL = ShaderGenerator::getSingleton().getTargetLanguage() == "hlsl";

    if (isHLSL)
    {
        // set hlsl shader to use row-major matrices instead of column-major.
        vsProgram->setUseColumnMajorMatrices(false);
    }

    int numMonitors = mViewportGrid.x * mViewportGrid.y;
    vsProgram->addPreprocessorDefines(StringUtil::format("NUM_MONITORS=%d", numMonitors));

    vsProgram->addDependency("SampleLib_InstancedViewports");

    if(!mLayeredTarget)
        psProgram->addDependency("SampleLib_InstancedViewports");

    Function* vsMain = vsProgram->getEntryPointFunction();
    Function* psMain = psProgram->getEntryPointFunction();

    if (mLayeredTarget)
    {
        mVSInMatrixArray = vsProgram->resolveParameter(GpuProgramParameters::ACT_WORLDVIEWPROJ_MATRIX_ARRAY, numMonitors);
    }
    else
    {
        mVSInMatrixArray = vsProgram->resolveParameter(GCT_MATRIX_4X4, -1, GPV_GLOBAL, "matrixArray", numMonitors);
    }

    // Resolve vertex shader output position in projective space.
    auto positionIn = vsMain->resolveInputParameter(Parameter::SPC_POSITION_OBJECT_SPACE);
    auto vsInMonitorIndex = vsMain->resolveInputParameter(Parameter::SPC_TEXTURE_COORDINATE1, GCT_FLOAT4);

    auto originalOutPositionProjectiveSpace = vsMain->resolveOutputParameter(Parameter::SPC_POSITION_PROJECTIVE_SPACE);
    auto vsOutPositionProjectiveSpace = vsMain->resolveOutputParameter(Parameter::SPC_CUSTOM_CONTENT_BEGIN + 1, GCT_FLOAT4);

    auto vsOutMonitorIndex = vsMain->resolveOutputParameter(Parameter::SPC_TEXTURE_COORDINATE1, GCT_FLOAT4);

    // Add vertex shader invocations.
    auto vstage = vsMain->getStage(FFP_VS_TRANSFORM + 1);

    auto layerIdx = vsMain->resolveLocalParameter(GCT_INT1, "layer");
    vstage.callFunction("SGX_InstancedViewportsGetLayer", vsInMonitorIndex, layerIdx);
    vstage.callBuiltin("mul", {In(mVSInMatrixArray), At(layerIdx), In(positionIn), Out(originalOutPositionProjectiveSpace)});

    // Output position in projective space.
    vstage.assign(originalOutPositionProjectiveSpace, vsOutPositionProjectiveSpace);

    if(mLayeredTarget)
    {
        auto layer = vsMain->resolveOutputParameter(Parameter::SPC_LAYER);
        vstage.assign(layerIdx, layer);
        return true;
    }

    mPSInMonitorsCount = psProgram->resolveParameter(GCT_FLOAT2, "monitorsCount");

    // Output monitor index.
    vstage.assign(vsInMonitorIndex, vsOutMonitorIndex);

    // Add pixel shader invocations.
    auto psInMonitorIndex = psMain->resolveInputParameter(vsOutMonitorIndex);
    auto psInPositionProjectiveSpace = psMain->resolveInputParameter(vsOutPositionProjectiveSpace);
    auto fstage = psMain->getStage(FFP_PS_PRE_PROCESS + 1);
    fstage.callFunction("SGX_InstancedViewportsDiscardOutOfBounds",
                        {In(mPSInMonitorsCount), In(psInMonitorIndex), In(psInPositionProjectiveSpace)});

    return true;
}

//-----------------------------------------------------------------------
void ShaderExInstancedViewports::updateGpuProgramsParams(Renderable* rend, const Pass* pass, const AutoParamDataSource* source, const LightList* pLightList)
{
    if(mLayeredTarget)
        return; // using autoconstant

    if (mMonitorsCountChanged)
    {
        mPSInMonitorsCount->setGpuParameter(mViewportGrid);

        mMonitorsCountChanged = false;
    }

    Matrix4 shift = Matrix4::IDENTITY;
    shift.setScale(Vector3(1./mViewportGrid[0], 1./mViewportGrid[1], 1));

    std::vector<Matrix4> matrixArray;
    Vector2 monitorCount = mViewportGrid;

    int camIdx = 0;
    for (int x = 0 ; x < monitorCount.x ; x++)
        for (int y = 0 ; y < monitorCount.y ; y++)
        {
            auto worldViewProjectionMatrix = source->getWorldViewProjMatrix(camIdx++);

            shift.setTrans(Vector3((2*x - 1)/monitorCount[0], (2*y - 1)/monitorCount[1], 0));
            matrixArray.push_back(shift * worldViewProjectionMatrix);
        }

    // Update the matrix array
    mVSInMatrixArray->setGpuParameter((Real*)matrixArray.data(), matrixArray.size(), 16);
}
//-----------------------------------------------------------------------

void ShaderExInstancedViewports::setParameter(const String& name, const Any& value)
{
    if (name == "viewportGrid")
    {
        setMonitorsCount(any_cast<Vector2>(value));
        return;
    }

    if (name == "layeredTarget")
    {
        mLayeredTarget = any_cast<bool>(value);
        return;
    }

    if (name == "schemeName")
    {
        mSchemeName = any_cast<String>(value);
        return;
    }

    SubRenderState::setParameter(name, value);
}

void ShaderExInstancedViewports::setMonitorsCount( const Vector2 monitorCount )
{
    mViewportGrid = monitorCount;
    mMonitorsCountChanged = true;

    Ogre::VertexDeclaration* vertexDeclaration = Ogre::HardwareBufferManager::getSingleton().createVertexDeclaration();
    vertexDeclaration->addElement(0, 0, Ogre::VET_FLOAT4, Ogre::VES_TEXTURE_COORDINATES, 1);

    Ogre::HardwareVertexBufferSharedPtr vbuf =
        Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        vertexDeclaration->getVertexSize(0), monitorCount.x * monitorCount.y, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    vbuf->setInstanceDataStepRate(1);
    vbuf->setIsInstanceData(true);

    Vector4f* buf = (Vector4f *)vbuf->lock(Ogre::HardwareBuffer::HBL_DISCARD);
    int i = 0;
    for (int x = 0 ; x < monitorCount.x ; x++)
        for (int y = 0 ; y < monitorCount.y ; y++)
        {
            *buf++ = Vector4f(x, y, i++, 0);
        }
    vbuf->unlock();

    mOwnsGlobalData = true;

    auto rs = Ogre::Root::getSingleton().getRenderSystem();
    rs->enableSchemeInstancing(mSchemeName, vbuf, vertexDeclaration, monitorCount.x * monitorCount.y);
}
ShaderExInstancedViewports::~ShaderExInstancedViewports()
{
    if (!mOwnsGlobalData)
        return;

    auto rs = Ogre::Root::getSingleton().getRenderSystem();

    if (auto decl = rs->getSchemeInstancingData(rs->_getDefaultViewportMaterialScheme()).vertexDecl)
    {
        Ogre::HardwareBufferManager::getSingleton().destroyVertexDeclaration(decl);
    }

    rs->disableSchemeInstancing(mSchemeName);
}

//-----------------------------------------------------------------------
const String& ShaderExInstancedViewportsFactory::getType() const
{
    return ShaderExInstancedViewports::Type;
}


//-----------------------------------------------------------------------
SubRenderState* ShaderExInstancedViewportsFactory::createInstanceImpl()
{
    return OGRE_NEW ShaderExInstancedViewports;
}

//-----------------------------------------------------------------------

}
}

#endif
