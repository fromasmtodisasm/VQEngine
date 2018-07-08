//	Copyright(C) 2018  - Volkan Ilbeyli
//
//	This program is free software : you can redistribute it and / or modify
//	it under the terms of the GNU General Public License as published by
//	the Free Software Foundation, either version 3 of the License, or
//	(at your option) any later version.
//
//	This program is distributed in the hope that it will be useful,
//	but WITHOUT ANY WARRANTY; without even the implied warranty of
//	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
//	GNU General Public License for more details.
//
//	You should have received a copy of the GNU General Public License
//	along with this program.If not, see <http://www.gnu.org/licenses/>.
//
//	Contact: volkanilbeyli@gmail.com

// skeleton code for transparency, detailed implementation will be done later on.
// disable it for now.
#define ENABLE_TRANSPARENCY 0

#include "Engine.h"

#include "Application/Input.h"
#include "Application/ThreadPool.h"

#include "Utilities/Log.h"
#include "Utilities/PerfTimer.h"
#include "Utilities/CustomParser.h"
#include "Utilities/Profiler.h"
#include "Utilities/Camera.h"

#include "Renderer/Renderer.h"
#include "Renderer/TextRenderer.h"
#include "Renderer/GeometryGenerator.h"

#include "Scenes/ObjectsScene.h"
#include "Scenes/SSAOTestScene.h"
#include "Scenes/IBLTestScene.h"
#include "Scenes/StressTestScene.h"
#include "Scenes/SponzaScene.h"

#include <sstream>
#include <DirectXMath.h>

using namespace VQEngine;

Settings::Engine Engine::sEngineSettings;
Engine* Engine::sInstance = nullptr;
Engine * Engine::GetEngine()
{
	if (sInstance == nullptr){sInstance = new Engine();}
	return sInstance;
}
void Engine::ToggleRenderingPath()
{
	mbUseDeferredRendering = !mbUseDeferredRendering;

	// initialize GBuffer if its not initialized, i.e., 
	// Renderer started in forward mode and we're toggling deferred for the first time
	if (!mDeferredRenderingPasses._GBuffer.bInitialized && mbUseDeferredRendering)
	{
		mDeferredRenderingPasses.InitializeGBuffer(mpRenderer);
	}
	Log::Info("Toggle Rendering Path: %s Rendering enabled", mbUseDeferredRendering ? "Deferred" : "Forward");

	// if we just turned deferred rendering off, clear the gbuffer textures
	if (!mbUseDeferredRendering)
	{
		mDeferredRenderingPasses.ClearGBuffer(mpRenderer);
		mSelectedShader = sEngineSettings.rendering.bUseBRDFLighting ? EShaders::FORWARD_BRDF : EShaders::FORWARD_PHONG;
	}
}
void Engine::ToggleAmbientOcclusion()
{
	mbIsAmbientOcclusionOn = !mbIsAmbientOcclusionOn;
	if (!mbIsAmbientOcclusionOn)
	{
		mDeferredRenderingPasses.ClearGBuffer(mpRenderer);
	}
	Log::Info("Toggle Ambient Occlusion: %s", mbIsAmbientOcclusionOn ? "On" : "Off");
}

void Engine::ToggleBloom()
{
	mbIsBloomOn = !mbIsBloomOn;
	Log::Info("Toggle Bloom: %s", mbIsBloomOn ? "On" : "Off");
}

float Engine::GetTotalTime() const { return mpTimer->TotalTime(); }

Engine::Engine()
	: mpRenderer(new Renderer())
	, mpTextRenderer(new TextRenderer())
	, mpInput(new Input())
	, mpTimer(new PerfTimer()) 
	, mpCPUProfiler(new CPUProfiler())
	, mpGPUProfiler(new GPUProfiler())
	, mbUsePaniniProjection(false)
	, mbShowControls(true)
	, mFrameCount(0)
{}

Engine::~Engine(){}


bool Engine::Initialize(HWND hwnd)
{
	mpTimer->Start();
	if (!mpRenderer || !mpInput || !mpTimer)
	{
		Log::Error("Nullptr Engine::Init()\n");
		return false;
	}

	// INITIALIZE SYSTEMS
	//--------------------------------------------------------------
	const bool bEnableLogging = true;	// todo: read from settings
	const Settings::Rendering& rendererSettings = sEngineSettings.rendering;
	const Settings::Window& windowSettings = sEngineSettings.window;

	mpInput->Initialize();
	if (!mpRenderer->Initialize(hwnd, windowSettings))
	{
		Log::Error("Cannot initialize Renderer.\n");
		return false;
	}

	// PRIMITIVES
	//--------------------------------------------------------------------
	{
		// cylinder parameters
		const float	 cylHeight = 3.1415f;
		const float	 cylTopRadius = 1.0f;
		const float	 cylBottomRadius = 1.0f;
		const unsigned cylSliceCount = 120;
		const unsigned cylStackCount = 100;

		// grid parameters
		const float gridWidth = 1.0f;
		const float gridDepth = 1.0f;
		const unsigned gridFinenessH = 100;
		const unsigned gridFinenessV = 100;

		// sphere parameters
		const float sphRadius = 2.0f;
		const unsigned sphRingCount = 25;
		const unsigned sphSliceCount = 25;

		mBuiltinMeshes =	// this should match enum declaration order
		{
			GeometryGenerator::Triangle(1.0f),
			GeometryGenerator::Quad(1.0f),
			GeometryGenerator::Cube(),
			GeometryGenerator::Cylinder(cylHeight, cylTopRadius, cylBottomRadius, cylSliceCount, cylStackCount),
			GeometryGenerator::Sphere(sphRadius, sphRingCount, sphSliceCount),
			GeometryGenerator::Grid(gridWidth, gridDepth, gridFinenessH, gridFinenessV),
			GeometryGenerator::Sphere(sphRadius / 40, 10, 10),
		};
	}

	if (!mpTextRenderer->Initialize(mpRenderer))
	{
		Log::Error("Cannot initialize Text Renderer.\n");
		return false;
	}
	mpGPUProfiler->Init(mpRenderer->m_deviceContext, mpRenderer->m_device);

	// INITIALIZE RENDERING
	//--------------------------------------------------------------
	// render passes
	mbUseDeferredRendering = rendererSettings.bUseDeferredRendering;
	mbIsAmbientOcclusionOn = rendererSettings.bAmbientOcclusion;
	mbIsBloomOn = true;	// currently not deserialized
	mDisplayRenderTargets = true;
	mSelectedShader = mbUseDeferredRendering ? mDeferredRenderingPasses._geometryShader : EShaders::FORWARD_BRDF;
	mWorldDepthTarget = 0;	// assumes first index in renderer->m_depthTargets[]


	mpScenes.push_back(new ObjectsScene(mpRenderer, mpTextRenderer));
	mpScenes.push_back(new SSAOTestScene(mpRenderer, mpTextRenderer));
	mpScenes.push_back(new IBLTestScene(mpRenderer, mpTextRenderer));
	mpScenes.push_back(new StressTestScene(mpRenderer, mpTextRenderer));
	mpScenes.push_back(new SponzaScene(mpRenderer, mpTextRenderer));

	mpTimer->Stop();
	Log::Info("Engine initialized in %.2fs", mpTimer->DeltaTime());
	return true;
}

void Engine::Exit()
{
	mpGPUProfiler->Exit();
	mpTextRenderer->Exit();
	mpRenderer->Exit();
	
	mBuiltinMeshes.clear();

	Log::Exit();
	if (sInstance)
	{
		delete sInstance;
		sInstance = nullptr;
	}
}

#define OVERRIDE_LEVEL_LOAD 1	// Toggle for overriding level loading
#define OVERRIDE_LEVEL_VALUE 0	// which level to load
bool Engine::Load(ThreadPool* pThreadPool)
{
	mpThreadPool = pThreadPool;
	const Settings::Rendering& rendererSettings = sEngineSettings.rendering;
	
	RenderLoadingScreen();

	// LOAD ENVIRONMENT MAPS
	//
	mpCPUProfiler->BeginProfile();
	mpCPUProfiler->BeginEntry("EngineLoad");
	Log::Info("-------------------- LOADING ENVIRONMENT MAPS --------------------- ");
	mpTimer->Start();
	Skybox::InitializePresets(mpRenderer, rendererSettings.bEnableEnvironmentLighting, rendererSettings.bPreLoadEnvironmentMaps);
	mpTimer->Stop();
	Log::Info("-------------------- ENVIRONMENT MAPS LOADED IN %.2fs. --------------------", mpTimer->DeltaTime());
	mpTimer->Reset();

	// SCENE INITIALIZATION
	//
	Log::Info("-------------------- LOADING SCENE --------------------- ");
	mpTimer->Start();
	const size_t numScenes = sEngineSettings.sceneNames.size();
	assert(sEngineSettings.levelToLoad < numScenes);
#if defined(_DEBUG) && (OVERRIDE_LEVEL_LOAD > 0)
	sEngineSettings.levelToLoad = OVERRIDE_LEVEL_VALUE;
#endif
	if (!LoadSceneFromFile())
	{
		Log::Error("Engine couldn't load scene.");
		return false;
	}
	mpTimer->Stop();
	Log::Info("-------------------- SCENE LOADED IN %.2fs. --------------------", mpTimer->DeltaTime());
	mpTimer->Reset();

	// RENDER PASS INITIALIZATION
	//
	mpTimer->Start();
	{	
		Log::Info("---------------- INITIALIZING RENDER PASSES ---------------- ");
		mShadowMapPass.Initialize(mpRenderer, mpRenderer->m_device, rendererSettings.shadowMap);
		//renderer->m_Direct3D->ReportLiveObjects();
		
		mDeferredRenderingPasses.Initialize(mpRenderer);
		mPostProcessPass.Initialize(mpRenderer, rendererSettings.postProcess);
		mDebugPass.Initialize(mpRenderer);
		mSSAOPass.Initialize(mpRenderer);

		// Samplers
		D3D11_SAMPLER_DESC normalSamplerDesc = {};
		normalSamplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
		normalSamplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
		normalSamplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		normalSamplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		normalSamplerDesc.MinLOD = 0.f;
		normalSamplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		normalSamplerDesc.MipLODBias = 0.f;
		normalSamplerDesc.MaxAnisotropy = 0;
		normalSamplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
		mNormalSampler = mpRenderer->CreateSamplerState(normalSamplerDesc);
	}
	mpTimer->Stop();
	Log::Info("---------------- INITIALIZING RENDER PASSES DONE IN %.2fs ---------------- ", mpTimer->DeltaTime());
	mpCPUProfiler->EndEntry();
	mpCPUProfiler->EndProfile();


	// Thread Pool Unit Test ------------------------------------------------
	if(false)
	{
		constexpr long long sz = 40000000;
		auto sumRnd = [&]() 
		{
			std::vector<long long> nums(sz, 0);
			for (int i = 0; i < sz; ++i)
			{
				nums[i] = RandI(0, 5000);
			}
			unsigned long long result = 0;
			for (int i = 0; i < sz; ++i)
			{
				if (nums[i] > 3000)
					result += nums[i];
			}
			return result;
		};
		auto sum = [&]()
		{
			std::vector<long long> nums(sz, 0);
			for (int i = 0; i < sz; ++i)
			{
				nums[i] = RandI(0, 5000);
			}
			unsigned long long result = 0;
			for (int i = 0; i < sz; ++i)
			{
				result += nums[i];
			}
			return result;
		};

		constexpr int threadCount = 16;
		std::future<unsigned long long> futures[threadCount] =
		{
			mpThreadPool->AddTask(sumRnd),
			mpThreadPool->AddTask(sumRnd),
			mpThreadPool->AddTask(sumRnd),
			mpThreadPool->AddTask(sumRnd),
			mpThreadPool->AddTask(sumRnd),
			mpThreadPool->AddTask(sumRnd),
			mpThreadPool->AddTask(sumRnd),
			mpThreadPool->AddTask(sumRnd),

			mpThreadPool->AddTask(sum),
			mpThreadPool->AddTask(sum),
			mpThreadPool->AddTask(sum),
			mpThreadPool->AddTask(sum),
			mpThreadPool->AddTask(sum),
			mpThreadPool->AddTask(sum),
			mpThreadPool->AddTask(sum),
			mpThreadPool->AddTask(sum),
		};

		std::vector<unsigned long long> results;
		unsigned long long total = 0;
		std::for_each(std::begin(futures), std::end(futures), [&](decltype(futures[0]) f)
		{
			results.push_back(f.get());
			total += results.back();
		});

		std::string strResult = "total (" + std::to_string(total) + ") = ";
		for (int i = 0; i < threadCount; ++i)
		{
			strResult += "(" + std::to_string(results[i]) + ") " + (i == threadCount-1 ? "" : " + ");
		}
		Log::Info(strResult);
	}
	// Thread Pool Unit Test ------------------------------------------------

	return true;
}


bool Engine::LoadSceneFromFile()
{
	mCurrentLevel = sEngineSettings.levelToLoad;
	SerializedScene mSerializedScene = Parser::ReadScene(mpRenderer, sEngineSettings.sceneNames[mCurrentLevel]);
	if (mSerializedScene.loadSuccess == '0')
	{
		Log::Error("Scene[%d] did not load.", mCurrentLevel);
		return false;
	}

	assert(mCurrentLevel < mpScenes.size());
	mpActiveScene = mpScenes[mCurrentLevel];
	mpActiveScene->mpThreadPool = mpThreadPool;

	mpActiveScene->LoadScene(mSerializedScene, sEngineSettings.window, mBuiltinMeshes);

	// update engine settings and post process settings
	// todo: multiple data - inconsistent state -> sort out ownership
	sEngineSettings.rendering.postProcess.bloom = mSerializedScene.settings.bloom;
	mPostProcessPass._settings = sEngineSettings.rendering.postProcess;
	//mShadowMapPass._shadowViewportDirectional.Width = mShadowMapPass._shadowViewportDirectional.Height = mpActiveScene->mDirectionalLight.shadowMapSize.x();
	return true;
}

bool Engine::LoadScene(int level)
{
	mpActiveScene->UnloadScene();
	mShadowCasters.clear();
	Engine::sEngineSettings.levelToLoad = level;
	const bool bSceneLoaded = LoadSceneFromFile();
	if (bSceneLoaded)
	{
		// reinitialize shadow pass



		return true;
	}
	else return false;
}


bool Engine::UpdateAndRender()
{
	const float dt = mpTimer->Tick();
	mpCPUProfiler->BeginEntry("CPU");

	const bool bExitApp = !HandleInput();
	if (!mbIsPaused)
	{
		CalcFrameStats(dt);

		mpCPUProfiler->BeginEntry("Update()");
		mpActiveScene->UpdateScene(dt);
		mpCPUProfiler->EndEntry();

		PreRender();
		Render();
	}

	mpCPUProfiler->EndEntry();

	mpCPUProfiler->StateCheck();
	return bExitApp;
}


bool Engine::HandleInput()
{
	if (mpInput->IsKeyTriggered("Backspace"))	TogglePause();

	if (mpInput->IsKeyTriggered("F1")) ToggleRenderingPath();
	if (mpInput->IsKeyTriggered("F2")) ToggleAmbientOcclusion();
	if (mpInput->IsKeyTriggered("F3")) ToggleBloom();
	if (mpInput->IsKeyTriggered("F4")) mDisplayRenderTargets = !mDisplayRenderTargets;


	if (mpInput->IsKeyTriggered(";"))
	{
		if (mpInput->IsKeyDown("Shift"));
		else								ToggleProfilerRendering();
	}
	if (mpInput->IsKeyTriggered("'"))
	{
		if (mpInput->IsKeyDown("Shift"))	ToggleControlsTextRendering();
		else								ToggleRenderingStats();
	}


	if (mpInput->IsKeyTriggered("\\")) mpRenderer->ReloadShaders();

#if SSAO_DEBUGGING
	// todo: wire this to some UI text/control
	if (mbIsAmbientOcclusionOn)
	{
		if (mpInput->IsKeyDown("Shift"))
		{
			const float step = 0.1f;
			if (mpInput->IsScrollUp()) { mSSAOPass.intensity += step; Log::Info("SSAO Intensity: %.2f", mSSAOPass.intensity); }
			if (mpInput->IsScrollDown()) { mSSAOPass.intensity -= step; if (mSSAOPass.intensity < 0.301) mSSAOPass.intensity = 1.0f; Log::Info("SSAO Intensity: %.2f", mSSAOPass.intensity); }
		}
		else
		{
			const float step = 0.5f;
			if (mpInput->IsScrollUp()) { mSSAOPass.radius += step; Log::Info("SSAO Radius: %.2f", mSSAOPass.radius); }
			if (mpInput->IsScrollDown()) { mSSAOPass.radius -= step; if (mSSAOPass.radius < 0.301) mSSAOPass.radius = 1.0f; Log::Info("SSAO Radius: %.2f", mSSAOPass.radius); }
		}
	}
#endif

#if BLOOM_DEBUGGING
	if (mbIsBloomOn)
	{
		Settings::Bloom& bloom = mPostProcessPass._settings.bloom;
		float& threshold = bloom.brightnessThreshold;
		const float step = 0.05f;
		const float threshold_hi = 3.0f;
		const float threshold_lo = 0.05f;
		if (mpInput->IsScrollUp()) { threshold += step; if (threshold > threshold_hi) threshold = threshold_hi; Log::Info("Bloom Brightness Cutoff Threshold: %.2f", threshold); }
		if (mpInput->IsScrollDown()) { threshold -= step; if (threshold < threshold_lo) threshold = threshold_lo; Log::Info("Bloom Brightness Cutoff Threshold: %.2f", threshold); }
	}
#endif

	// SCENE -------------------------------------------------------------
	if (mpInput->IsKeyTriggered("R"))
	{
		if (mpInput->IsKeyDown("Shift")) ReloadScene();
		else							 mpActiveScene->ResetActiveCamera();
	}

	if (mpInput->IsKeyTriggered("1"))	LoadScene(0);
	if (mpInput->IsKeyTriggered("2"))	LoadScene(1);
	if (mpInput->IsKeyTriggered("3"))	LoadScene(2);
	if (mpInput->IsKeyTriggered("4"))	LoadScene(3);
	if (mpInput->IsKeyTriggered("5"))	LoadScene(4);


	// index using enums. first element of environment map presets starts with cubemap preset count, as if both lists were concatenated.
	const EEnvironmentMapPresets firstPreset = static_cast<EEnvironmentMapPresets>(CUBEMAP_PRESET_COUNT);
	const EEnvironmentMapPresets lastPreset = static_cast<EEnvironmentMapPresets>(
		static_cast<EEnvironmentMapPresets>(CUBEMAP_PRESET_COUNT) + ENVIRONMENT_MAP_PRESET_COUNT - 1
		);

	EEnvironmentMapPresets selectedEnvironmentMap = mpActiveScene->GetActiveEnvironmentMapPreset();
	if (ENGINE->INP()->IsKeyTriggered("PageUp"))	selectedEnvironmentMap = selectedEnvironmentMap == lastPreset ? firstPreset : static_cast<EEnvironmentMapPresets>(selectedEnvironmentMap + 1);
	if (ENGINE->INP()->IsKeyTriggered("PageDown"))	selectedEnvironmentMap = selectedEnvironmentMap == firstPreset ? lastPreset : static_cast<EEnvironmentMapPresets>(selectedEnvironmentMap - 1);
	if (ENGINE->INP()->IsKeyTriggered("PageUp") || ENGINE->INP()->IsKeyTriggered("PageDown"))
		mpActiveScene->SetEnvironmentMap(selectedEnvironmentMap);

	return true;
}

void Engine::CalcFrameStats(float dt)
{
	static unsigned long long frameCount = 0;
	constexpr size_t RefreshRate = 5;	// refresh every 5 frames
	constexpr size_t SampleCount = 50;	// over 50 dt samples

	static std::vector<float> dtSamples(SampleCount, 0.0f);

	dtSamples[frameCount % SampleCount] = dt;

	float dtSampleSum = 0.0f;
	std::for_each(dtSamples.begin(), dtSamples.end(), [&dtSampleSum](float dt) { dtSampleSum += dt; });
	const float frameTime = dtSampleSum / SampleCount;
	const int fps = static_cast<int>(1.0f / frameTime);


	if (frameCount % RefreshRate == 0)
	{
		mCurrentFrameTime = frameTime;
		float frameTimeGPU = mpGPUProfiler->GetEntry("GPU");

		std::ostringstream stats;
		stats.precision(2);
		stats << std::fixed;
		stats << "VQEngine Demo | " << "CPU: " << frameTime * 1000.0f << " ms  GPU: " << frameTimeGPU * 1000.f << " ms | FPS: ";
		stats.precision(4);
		stats << fps;
		SetWindowText(mpRenderer->GetWindow(), stats.str().c_str());
	}

	++frameCount;
}

const Settings::Engine& Engine::ReadSettingsFromFile()
{
	sEngineSettings = Parser::ReadSettings("EngineSettings.ini");
	return sEngineSettings;
}

bool Engine::ReloadScene()
{
	const auto& settings = Engine::GetSettings();
	Log::Info("Reloading Scene (%d)...", settings.levelToLoad);

	mpActiveScene->UnloadScene();
	mShadowCasters.clear();

	return LoadSceneFromFile();
}


void Engine::PreRender()
{
	mpCPUProfiler->BeginEntry("PreRender()");
	
	// set scene view
	const Camera& viewCamera = mpActiveScene->GetActiveCamera();
	const XMMATRIX view = viewCamera.GetViewMatrix();
	const XMMATRIX viewInverse = viewCamera.GetViewInverseMatrix();
	const XMMATRIX proj = viewCamera.GetProjectionMatrix();
	XMVECTOR det = XMMatrixDeterminant(proj);
	const XMMATRIX projInv = XMMatrixInverse(&det, proj);

	mSceneView.viewProj = view * proj;
	mSceneView.view = view;
	mSceneView.viewInverse = viewInverse;
	mSceneView.projection = proj;
	mSceneView.projectionInverse = projInv;

	mSceneView.cameraPosition = viewCamera.GetPositionF();

	mSceneView.sceneRenderSettings = mpActiveScene->GetSceneRenderSettings();
	mSceneView.bIsPBRLightingUsed = IsLightingModelPBR();
	mSceneView.bIsDeferredRendering = mbUseDeferredRendering;
	mSceneView.bIsIBLEnabled = mpActiveScene->mSceneRenderSettings.bSkylightEnabled && mSceneView.bIsPBRLightingUsed;

	mSceneView.environmentMap = mpActiveScene->GetEnvironmentMap();

	// gather scene lights
	mpActiveScene->GatherLightData(mSceneLightData, mShadowView);
	

	// TODO: #RenderPass or Scene should manage this.
	// mTBNDrawObjects.clear();
	// std::vector<const GameObject*> objects;
	// mpActiveScene->GetSceneObjects(objects);
	// for (const GameObject* obj : objects)
	// {
	// 	if (obj->mRenderSettings.bRenderTBN)
	// 		mTBNDrawObjects.push_back(obj);
	// }

	mShadowCasters.clear();
	mpActiveScene->GatherShadowCasters(mShadowCasters);

	mpActiveScene->PreRender();

	mpCPUProfiler->EndEntry();
}

void Engine::SendLightData() const
{
	const float shadowDimension = static_cast<float>(mShadowMapPass._shadowMapDimension);

	// LIGHTS ( POINT | SPOT | DIRECTIONAL )
	//
	mpRenderer->SetConstantStruct("Lights", &mSceneLightData._cb);
	mpRenderer->SetConstant2f("spotShadowMapDimensions", vec2(shadowDimension, shadowDimension));

	// SHADOW MAPS
	//
	mpRenderer->SetTextureArray("texSpotShadowMaps", mShadowMapPass._spotShadowMaps);
	mpRenderer->SetTextureArray("texDirectionalShadowMaps", mShadowMapPass._directionalShadowMaps);

#ifdef _DEBUG
	const SceneLightingData::cb& cb = mSceneLightData._cb;	// constant buffer shorthand
	if (cb.pointLightCount > cb.pointLights.size())	OutputDebugString("Warning: light count larger than MAX_LIGHTS\n");
	if (cb.spotLightCount  > cb.spotLights.size())	OutputDebugString("Warning: spot count larger than MAX_SPOTS\n");
#endif
}

void Engine::Render()
{
	mpCPUProfiler->BeginEntry("Render()");

	mpGPUProfiler->BeginFrame(mFrameCount);
	mpGPUProfiler->BeginQuery("GPU");

	mpRenderer->BeginFrame();

	const XMMATRIX& viewProj = mSceneView.viewProj;
	const bool bSceneSSAO = mSceneView.sceneRenderSettings.ssao.bEnabled;

	// SHADOW MAPS
	//------------------------------------------------------------------------
	mpCPUProfiler->BeginEntry("Shadow Pass");
	mpGPUProfiler->BeginQuery("Shadow Pass");
	mpRenderer->BeginEvent("Shadow Pass");
	
	mpRenderer->UnbindRenderTargets();	// unbind the back render target | every pass has their own render targets
	mShadowMapPass.RenderShadowMaps(mpRenderer, mShadowCasters, mShadowView);
	
	mpRenderer->EndEvent();
	mpCPUProfiler->EndEntry();
	mpGPUProfiler->EndQuery();

	// LIGHTING PASS
	//------------------------------------------------------------------------
	mpRenderer->ResetPipelineState();
	mpRenderer->SetRasterizerState(static_cast<int>(EDefaultRasterizerState::CULL_NONE));
	mpRenderer->SetViewport(mpRenderer->WindowWidth(), mpRenderer->WindowHeight());

	//==========================================================================
	// DEFERRED RENDERER
	//==========================================================================
	if (mbUseDeferredRendering)
	{
		const GBuffer& gBuffer = mDeferredRenderingPasses._GBuffer;
		const TextureID texNormal = mpRenderer->GetRenderTargetTexture(gBuffer._normalRT);
		const TextureID texDiffuseRoughness = mpRenderer->GetRenderTargetTexture(gBuffer._diffuseRoughnessRT);
		const TextureID texSpecularMetallic = mpRenderer->GetRenderTargetTexture(gBuffer._specularMetallicRT);
		const TextureID texDepthTexture = mpRenderer->mDefaultDepthBufferTexture;
		const TextureID tSSAO = mbIsAmbientOcclusionOn && bSceneSSAO
			? mpRenderer->GetRenderTargetTexture(mSSAOPass.blurRenderTarget)
			: mSSAOPass.whiteTexture4x4;

		// GEOMETRY - DEPTH PASS
		mpGPUProfiler->BeginQuery("Geometry Pass");
		mpCPUProfiler->BeginEntry("Geometry Pass");
		mpRenderer->BeginEvent("Geometry Pass");
		mDeferredRenderingPasses.SetGeometryRenderingStates(mpRenderer);
		mFrameStats.numSceneObjects = mpActiveScene->RenderOpaque(mSceneView);
		mpRenderer->EndEvent();	
		mpCPUProfiler->EndEntry();
		mpGPUProfiler->EndQuery();

		// AMBIENT OCCLUSION  PASS
		mpCPUProfiler->BeginEntry("SSAO Pass");
		mpGPUProfiler->BeginQuery("SSAO");
		if (mbIsAmbientOcclusionOn && bSceneSSAO)
		{
			// TODO: if BeginEntry() is inside, it's never reset to 0 if ambient occl is turned off
			mpGPUProfiler->BeginQuery("Occlusion");
			mpRenderer->BeginEvent("Ambient Occlusion Pass");
			mSSAOPass.RenderOcclusion(mpRenderer, texNormal, mSceneView);
			//m_SSAOPass.BilateralBlurPass(m_pRenderer);	// todo
			mpGPUProfiler->EndQuery();

			mpGPUProfiler->BeginQuery("Blur");
			mSSAOPass.GaussianBlurPass(mpRenderer);
			mpRenderer->EndEvent();	
			mpGPUProfiler->EndQuery();
		}
		mpCPUProfiler->EndEntry();
		mpGPUProfiler->EndQuery();

		// DEFERRED LIGHTING PASS
		mpCPUProfiler->BeginEntry("Lighting Pass");
		mpGPUProfiler->BeginQuery("Lighting Pass");
		mpRenderer->BeginEvent("Lighting Pass");
#if ENABLE_TRANSPARENCY
		mpGPUProfiler->BeginQuery("Opaque Pass (ScreenSpace)");
		mpCPUProfiler->BeginEntry("Opaque Pass (ScreenSpace)");
#endif
		{
			mDeferredRenderingPasses.RenderLightingPass(mpRenderer, mPostProcessPass._worldRenderTarget, mSceneView, mSceneLightData, tSSAO, sEngineSettings.rendering.bUseBRDFLighting);
		}
#if ENABLE_TRANSPARENCY
		mpCPUProfiler->EndEntry();
		mpGPUProfiler->EndQuery();
#endif

#if ENABLE_TRANSPARENCY
		// TRANSPARENT OBJECTS - FORWARD RENDER
		mpGPUProfiler->BeginQuery("Alpha Pass (Forward)");
		mpCPUProfiler->BeginEntry("Alpha Pass (Forward)");
		{
			mpRenderer->BindDepthTarget(GetWorldDepthTarget());
			mpRenderer->SetShader(EShaders::FORWARD_BRDF);
			mpRenderer->SetDepthStencilState(EDefaultDepthStencilState::DEPTH_STENCIL_WRITE);
			//mpRenderer->SetBlendState(EDefaultBlendState::ADDITIVE_COLOR);


			mpRenderer->SetConstant1f("ambientFactor", mSceneView.sceneRenderSettings.ambientFactor);
			mpRenderer->SetConstant3f("cameraPos", mSceneView.cameraPosition);
			mpRenderer->SetConstant2f("screenDimensions", mpRenderer->GetWindowDimensionsAsFloat2());
			const TextureID texIrradianceMap = mSceneView.environmentMap.irradianceMap;
			const SamplerID smpEnvMap = mSceneView.environmentMap.envMapSampler < 0 ? EDefaultSamplerState::POINT_SAMPLER : mSceneView.environmentMap.envMapSampler;
			const TextureID prefilteredEnvMap = mSceneView.environmentMap.prefilteredEnvironmentMap;
			const TextureID tBRDFLUT = mpRenderer->GetRenderTargetTexture(EnvironmentMap::sBRDFIntegrationLUTRT);
			const bool bSkylight = mSceneView.bIsIBLEnabled && texIrradianceMap != -1;
			if (bSkylight)
			{
				mpRenderer->SetTexture("tIrradianceMap", texIrradianceMap);
				mpRenderer->SetTexture("tPreFilteredEnvironmentMap", prefilteredEnvMap);
				mpRenderer->SetTexture("tBRDFIntegrationLUT", tBRDFLUT);
				mpRenderer->SetSamplerState("sEnvMapSampler", smpEnvMap);
			}

			mpRenderer->SetConstant1f("isEnvironmentLightingOn", bSkylight ? 1.0f : 0.0f);
			mpRenderer->SetSamplerState("sWrapSampler", EDefaultSamplerState::WRAP_SAMPLER);
			mpRenderer->SetSamplerState("sNearestSampler", EDefaultSamplerState::POINT_SAMPLER);
			mpRenderer->SetSamplerState("sLinearSampler", EDefaultSamplerState::LINEAR_FILTER_SAMPLER_WRAP_UVW);

			mpRenderer->Apply();
			mFrameStats.numSceneObjects += mpActiveScene->RenderAlpha(mSceneView);
			
		}
		mpCPUProfiler->EndEntry();
		mpGPUProfiler->EndQuery();
#endif
		mpRenderer->EndEvent();
		mpCPUProfiler->EndEntry();
		mpGPUProfiler->EndQuery();

		mpCPUProfiler->BeginEntry("Skybox & Lights");
		// LIGHT SOURCES
		mpRenderer->BindDepthTarget(mWorldDepthTarget);
		
		// SKYBOX
		//
		mpRenderer->SetDepthStencilState(EDefaultDepthStencilState::DEPTH_TEST_ONLY);
		mpRenderer->SetRasterizerState(EDefaultRasterizerState::CULL_NONE);
		if (mpActiveScene->HasSkybox())
		{
			mpActiveScene->RenderSkybox(mSceneView.viewProj);
		}

		RenderLights();
		mpCPUProfiler->EndEntry();
		mpRenderer->SetBlendState(EDefaultBlendState::DISABLED);
	}

	//==========================================================================
	// FORWARD RENDERER
	//==========================================================================
	else
	{
		const bool bZPrePass = mbIsAmbientOcclusionOn && bSceneSSAO;
		const TextureID tSSAO = bZPrePass ? mpRenderer->GetRenderTargetTexture(mSSAOPass.blurRenderTarget) : mSSAOPass.whiteTexture4x4;
		const TextureID texIrradianceMap = mSceneView.environmentMap.irradianceMap;
		const SamplerID smpEnvMap = mSceneView.environmentMap.envMapSampler < 0 ? EDefaultSamplerState::POINT_SAMPLER : mSceneView.environmentMap.envMapSampler;
		const TextureID prefilteredEnvMap = mSceneView.environmentMap.prefilteredEnvironmentMap;
		const TextureID tBRDFLUT = mpRenderer->GetRenderTargetTexture(EnvironmentMap::sBRDFIntegrationLUTRT);

		// AMBIENT OCCLUSION - Z-PREPASS
		if (bZPrePass)
		{
			const RenderTargetID normals	= mDeferredRenderingPasses._GBuffer._normalRT;
			const TextureID texNormal		= mpRenderer->GetRenderTargetTexture(normals);
			
			const bool bDoClearColor = true;
			const bool bDoClearDepth = true;
			const bool bDoClearStencil = false;
			ClearCommand clearCmd(
				bDoClearColor, bDoClearDepth, bDoClearStencil,
				{ 0, 0, 0, 0 }, 1, 0
			);


			mpGPUProfiler->BeginQuery("Z-PrePass");
			mpRenderer->BeginEvent("Z-PrePass");
			mpRenderer->SetShader(EShaders::Z_PREPRASS);
			mpRenderer->SetSamplerState("sNormalSampler", EDefaultSamplerState::LINEAR_FILTER_SAMPLER_WRAP_UVW);
			mpRenderer->BindRenderTarget(normals);
			mpRenderer->BindDepthTarget(mWorldDepthTarget);
			mpRenderer->SetDepthStencilState(EDefaultDepthStencilState::DEPTH_STENCIL_WRITE);
			mpRenderer->BeginRender(clearCmd);
			mpActiveScene->RenderOpaque(mSceneView);
			mpRenderer->EndEvent();
			mpGPUProfiler->EndQuery();

			mpGPUProfiler->BeginQuery("SSAO");
			mpRenderer->BeginEvent("Ambient Occlusion Pass");
			mpRenderer->SetDepthStencilState(EDefaultDepthStencilState::DEPTH_STENCIL_DISABLED);
			mpRenderer->UnbindRenderTargets();
			mpRenderer->Apply();

			mpGPUProfiler->BeginQuery("Occlusion");
			mSSAOPass.RenderOcclusion(mpRenderer, texNormal, mSceneView);
			mpGPUProfiler->EndQuery();

			mpGPUProfiler->BeginQuery("Blur");
			//m_SSAOPass.BilateralBlurPass(m_pRenderer);	// todo
			mSSAOPass.GaussianBlurPass(mpRenderer);
			mpGPUProfiler->EndQuery();
			mpGPUProfiler->EndQuery();	// SSAO
			mpRenderer->EndEvent();
		}

		const bool bDoClearColor = true;
		const bool bDoClearDepth = !bZPrePass;
		const bool bDoClearStencil = true;
		ClearCommand clearCmd(
			bDoClearColor, bDoClearDepth, bDoClearStencil,
			{ 0, 0, 0, 0 }, 1, 0
		);

		mpRenderer->BindRenderTarget(mPostProcessPass._worldRenderTarget);
		mpRenderer->BindDepthTarget(mWorldDepthTarget);
		mpRenderer->SetDepthStencilState(EDefaultDepthStencilState::DEPTH_WRITE);
		mpRenderer->BeginRender(clearCmd);

		// SKYBOX
		// if we're not rendering the skybox, call apply() to unbind
		// shadow light depth target so we can bind it in the lighting pass
		// otherwise, skybox render pass will take care of it
		if (mpActiveScene->HasSkybox())	mpActiveScene->RenderSkybox(mSceneView.viewProj);
		else
		{
			// todo: this might be costly, profile this
			mpRenderer->SetShader(mSelectedShader);	// set shader so apply won't complain 
			mpRenderer->Apply();					// apply to bind depth stencil
		}


		// LIGHTING
		mpRenderer->BeginEvent("Lighting Pass");
		mpRenderer->SetShader(mSelectedShader);
		if (mSelectedShader == EShaders::FORWARD_BRDF || mSelectedShader == EShaders::FORWARD_PHONG)
		{
			mpRenderer->SetTexture("texAmbientOcclusion", tSSAO);

			// todo: shader defines -> have a PBR shader with and without environment lighting through preprocessor
			const bool bSkylight = mSceneView.bIsIBLEnabled && texIrradianceMap != -1;
			//mpRenderer->SetSamplerState("sEnvMapSampler", smpEnvMap);
			if (bSkylight)
			{
				mpRenderer->SetTexture("tIrradianceMap", texIrradianceMap);
				mpRenderer->SetTexture("tPreFilteredEnvironmentMap", prefilteredEnvMap);
				mpRenderer->SetTexture("tBRDFIntegrationLUT", tBRDFLUT);
				mpRenderer->SetSamplerState("sEnvMapSampler", smpEnvMap);
			}
			
			if (mSelectedShader == EShaders::FORWARD_BRDF)
			{
				mpRenderer->SetConstant1f("isEnvironmentLightingOn", bSkylight ? 1.0f : 0.0f);
				mpRenderer->SetSamplerState("sWrapSampler", EDefaultSamplerState::WRAP_SAMPLER);
				mpRenderer->SetSamplerState("sNearestSampler", EDefaultSamplerState::POINT_SAMPLER);
			}
			else
				mpRenderer->SetSamplerState("sNormalSampler", EDefaultSamplerState::LINEAR_FILTER_SAMPLER_WRAP_UVW);
			// todo: shader defines -> have a PBR shader with and without environment lighting through preprocessor

			mpRenderer->SetConstant1f("ambientFactor", mSceneView.sceneRenderSettings.ssao.ambientFactor);
			mpRenderer->SetConstant3f("cameraPos", mSceneView.cameraPosition);
			mpRenderer->SetConstant2f("screenDimensions", mpRenderer->GetWindowDimensionsAsFloat2());
			mpRenderer->SetSamplerState("sLinearSampler", EDefaultSamplerState::LINEAR_FILTER_SAMPLER_WRAP_UVW);

			SendLightData();
		}

		mpActiveScene->RenderOpaque(mSceneView);

#if ENABLE_TRANSPARENCY
		mpRenderer->SetBlendState(EDefaultBlendState::ADDITIVE_COLOR);
		mpActiveScene->RenderAlpha(mSceneView);
		mpRenderer->SetBlendState(EDefaultBlendState::DISABLED);
#endif
		mpRenderer->EndEvent();

		RenderLights();
	}

	// Tangent-Bitangent-Normal drawing
	//------------------------------------------------------------------------
	const bool bIsShaderTBN = !mTBNDrawObjects.empty();
	if (bIsShaderTBN)
	{
		constexpr bool bSendMaterial = false;

		mpRenderer->BeginEvent("Draw TBN Vectors");
		if (mbUseDeferredRendering)
			mpRenderer->BindDepthTarget(mWorldDepthTarget);

		mpRenderer->SetShader(EShaders::TBN);

		for (const GameObject* obj : mTBNDrawObjects)
			obj->RenderOpaque(mpRenderer, mSceneView, bSendMaterial, mpActiveScene->mMaterials);
		

		if (mbUseDeferredRendering)
			mpRenderer->UnbindDepthTarget();

		mpRenderer->SetShader(mSelectedShader);
		mpRenderer->EndEvent();
	}

	// BOUNDING BOXES
	//
	mpActiveScene->RenderDebug(viewProj);
	
#if 1
	// POST PROCESS PASS
	//------------------------------------------------------------------------
	mpCPUProfiler->BeginEntry("Post Process");
	mpGPUProfiler->BeginQuery("Post Process");
	mPostProcessPass.Render(mpRenderer, mbIsBloomOn);
	mpCPUProfiler->EndEntry();
	mpGPUProfiler->EndQuery();

	// DEBUG PASS
	//------------------------------------------------------------------------
	if (mDisplayRenderTargets)
	{
		mpCPUProfiler->BeginEntry("Debug Textures");
		const int screenWidth  = sEngineSettings.window.width;
		const int screenHeight = sEngineSettings.window.height;
		const float aspectRatio = static_cast<float>(screenWidth) / screenHeight;

		// debug texture strip draw settings
		const int bottomPaddingPx = 0;	 // offset from bottom of the screen
		const int heightPx        = 128; // height for every texture
		const int paddingPx       = 0;	 // padding between debug textures
		const vec2 fullscreenTextureScaledDownSize((float)heightPx * aspectRatio, (float)heightPx);
		const vec2 squareTextureScaledDownSize    ((float)heightPx              , (float)heightPx);

		// Textures to draw
		const TextureID white4x4	 = mSSAOPass.whiteTexture4x4;
		//TextureID tShadowMap		 = mpRenderer->GetDepthTargetTexture(mShadowMapPass._spotShadowDepthTargets);
		TextureID tBlurredBloom		 = mpRenderer->GetRenderTargetTexture(mPostProcessPass._bloomPass._blurPingPong[0]);
		TextureID tDiffuseRoughness  = mpRenderer->GetRenderTargetTexture(mDeferredRenderingPasses._GBuffer._diffuseRoughnessRT);
		//TextureID tSceneDepth		 = m_pRenderer->m_state._depthBufferTexture._id;
		TextureID tSceneDepth		 = mpRenderer->GetDepthTargetTexture(0);
		TextureID tNormals			 = mpRenderer->GetRenderTargetTexture(mDeferredRenderingPasses._GBuffer._normalRT);
		TextureID tAO				 = mbIsAmbientOcclusionOn ? mpRenderer->GetRenderTargetTexture(mSSAOPass.blurRenderTarget) : mSSAOPass.whiteTexture4x4;
		TextureID tBRDF				 = mpRenderer->GetRenderTargetTexture(EnvironmentMap::sBRDFIntegrationLUTRT);
		TextureID preFilteredEnvMap  = mpActiveScene->GetEnvironmentMap().prefilteredEnvironmentMap;
		preFilteredEnvMap = preFilteredEnvMap < 0 ? white4x4 : preFilteredEnvMap; 

		const std::vector<DrawQuadOnScreenCommand> quadCmds = [&]() {

			// first row -----------------------------------------
			vec2 screenPosition(0.0f, static_cast<float>(bottomPaddingPx + heightPx * 0));
			std::vector<DrawQuadOnScreenCommand> c
			{	//		Pixel Dimensions	 Screen Position (offset below)	  Texture			DepthTexture?
				{ fullscreenTextureScaledDownSize,	screenPosition,			tSceneDepth			, true},
				{ fullscreenTextureScaledDownSize,	screenPosition,			tDiffuseRoughness	, false},
				{ fullscreenTextureScaledDownSize,	screenPosition,			tNormals			, false},
				//{ squareTextureScaledDownSize    ,	screenPosition,			tShadowMap			, true},
				{ fullscreenTextureScaledDownSize,	screenPosition,			tBlurredBloom		, false},
				{ fullscreenTextureScaledDownSize,	screenPosition,			tAO					, false},
				{ squareTextureScaledDownSize,		screenPosition,			tBRDF				, false },
			};
			for (size_t i = 1; i < c.size(); i++)	// offset textures accordingly (using previous' x-dimension)
				c[i].bottomLeftCornerScreenCoordinates.x() = c[i-1].bottomLeftCornerScreenCoordinates.x() + c[i - 1].dimensionsInPixels.x() + paddingPx;

			// second row -----------------------------------------
			screenPosition = vec2(0.0f, static_cast<float>(bottomPaddingPx + heightPx * 1));
			const size_t shadowMapCount = mSceneLightData._cb.pointLightCount_shadow;
			const size_t row_offset = c.size();
			size_t currShadowMap = 0;

			// the directional light
			{
				TextureID tex = mpRenderer->GetDepthTargetTexture(mShadowMapPass._directionalShadowDepthTarget);
				c.push_back({ squareTextureScaledDownSize, screenPosition, tex, false });

				if (currShadowMap > 0)
				{
					c[row_offset].bottomLeftCornerScreenCoordinates.x() 
						= c[row_offset - 1].bottomLeftCornerScreenCoordinates.x() 
						+ c[row_offset - 1].dimensionsInPixels.x() + paddingPx;
				}
				++currShadowMap;
			}

			// spot lights
			for (size_t i = 0; i < mSceneLightData._cb.spotLightCount_shadow; ++i)
			{
				TextureID tex = mpRenderer->GetDepthTargetTexture(mShadowMapPass._spotShadowDepthTargets[i]);
				c.push_back({
					squareTextureScaledDownSize, screenPosition, tex, false
					});

				if (currShadowMap > 0)
				{
					c[row_offset + i].bottomLeftCornerScreenCoordinates.x() 
						= c[row_offset + i - 1].bottomLeftCornerScreenCoordinates.x() 
						+ c[row_offset + i - 1].dimensionsInPixels.x() + paddingPx;
				}
				++currShadowMap;
			}

			return c;
		}();


		mpRenderer->BeginEvent("Debug Textures");
		mpRenderer->SetShader(EShaders::DEBUG);
		for (const DrawQuadOnScreenCommand& cmd : quadCmds)
		{
			mpRenderer->DrawQuadOnScreen(cmd);
		}
		mpRenderer->EndEvent();
		mpCPUProfiler->EndEntry();
	}

	// UI TEXT
	mpGPUProfiler->BeginQuery("UI");
	mpCPUProfiler->BeginEntry("UI");
	const int fps = static_cast<int>(1.0f / mCurrentFrameTime);

	std::ostringstream stats;
	std::string entry;

	TextDrawDescription drawDesc;
	drawDesc.color = LinearColor::green;
	drawDesc.scale = 0.35f;

	// Performance stats
	if (mbShowProfiler)
	{
		const bool bSortStats = true;
		float X_POS = sEngineSettings.window.width * 0.005f;
		float Y_POS = sEngineSettings.window.height * 0.05f;
		mpCPUProfiler->RenderPerformanceStats(mpTextRenderer, vec2(X_POS, Y_POS), drawDesc, bSortStats);

		drawDesc.color = LinearColor::cyan;
		Y_POS = sEngineSettings.window.height * 0.35f;
		mpGPUProfiler->RenderPerformanceStats(mpTextRenderer, vec2(X_POS, Y_POS), drawDesc, bSortStats);
	}
	if (mFrameStats.bShow)
	{
		drawDesc.color = vec3(0.0f, 0.7f, 1.0f);
		const float X_POS = sEngineSettings.window.width * 0.70f;
		const float Y_POS = sEngineSettings.window.height * 0.90f;
		mFrameStats.rstats = mpRenderer->GetRenderStats();
		mFrameStats.Render(mpTextRenderer, vec2(X_POS, Y_POS), drawDesc);
	}
	if (mbShowControls)
	{
		TextDrawDescription _drawDesc(drawDesc);
		_drawDesc.color = vec3(1, 1, 0.1f) * 0.65f;
		_drawDesc.scale = 0.35f;
		int numLine = FrameStats::numStat + 1;

		const float X_POS = sEngineSettings.window.width * 0.81f;
		const float Y_POS = 0.75f * sEngineSettings.window.height;
		const float LINE_HEIGHT = 25.0f;
		vec2 screenPosition(X_POS, Y_POS);
		
		_drawDesc.screenPosition = vec2(screenPosition.x(), screenPosition.y() + numLine++ * LINE_HEIGHT);
		_drawDesc.text = std::string("F1 - Render Mode: ") + (!mbUseDeferredRendering ? "Forward" : "Deferred");
		mpTextRenderer->RenderText(_drawDesc);

		_drawDesc.screenPosition = vec2(screenPosition.x(), screenPosition.y() + numLine++ * LINE_HEIGHT);
		_drawDesc.text = std::string("F2 - SSAO: ") + (mbIsAmbientOcclusionOn ? "On" : "Off");
		if(mbIsAmbientOcclusionOn)
			_drawDesc.text += (mSceneView.sceneRenderSettings.ssao.bEnabled ? " (Scene: On)" : " (Scene: Off)");
		mpTextRenderer->RenderText(_drawDesc);

		_drawDesc.screenPosition = vec2(screenPosition.x(), screenPosition.y() + numLine++ * LINE_HEIGHT);
		_drawDesc.text = std::string("F3 - Bloom: ") + (mbIsBloomOn ? "On" : "Off");
		if(mbIsBloomOn)
			_drawDesc.text += (mPostProcessPass._settings.bloom.bEnabled ? " (Scene: On)": " (Scene: Off)");
		mpTextRenderer->RenderText(_drawDesc);

		_drawDesc.screenPosition = vec2(screenPosition.x(), screenPosition.y() + numLine++ * LINE_HEIGHT);
		_drawDesc.text = std::string("F4 - Display Render Targets: ") + (mDisplayRenderTargets ? "On" : "Off");
		mpTextRenderer->RenderText(_drawDesc);
	}

	mpActiveScene->RenderUI();

	mpCPUProfiler->EndEntry();
	mpGPUProfiler->EndQuery();

#endif
	mpGPUProfiler->BeginQuery("Present");
	mpCPUProfiler->BeginEntry("Present");
	mpRenderer->EndFrame();
	mpCPUProfiler->EndEntry();
	mpGPUProfiler->EndQuery();


	mpCPUProfiler->EndEntry();	// Render() call

	mpGPUProfiler->EndQuery();
	mpGPUProfiler->EndFrame(mFrameCount);
	++mFrameCount;
}

void Engine::RenderLights() const
{
	mpRenderer->BeginEvent("Render Lights Pass");
	mpRenderer->SetShader(EShaders::UNLIT);
	for (const Light& light : mpActiveScene->mLights)
	{
		//if (!light._bEnabled) continue; // #BreaksRelease
		
		if (light._type == Light::ELightType::DIRECTIONAL)
			continue;	// do not render directional lights

		auto IABuffers = mBuiltinMeshes[light._renderMesh].GetIABuffers();

		mpRenderer->SetVertexBuffer(IABuffers.first);
		mpRenderer->SetIndexBuffer(IABuffers.second);
		const XMMATRIX world = light._transform.WorldTransformationMatrix();
		const XMMATRIX worldViewProj = world * mSceneView.viewProj;
		const vec3 color = light._color.Value();
		mpRenderer->SetConstant4x4f("worldViewProj", worldViewProj);
		mpRenderer->SetConstant3f("diffuse", color);
		mpRenderer->SetConstant1f("isDiffuseMap", 0.0f);
		mpRenderer->Apply();
		mpRenderer->DrawIndexed();
	}
	mpRenderer->EndEvent();
}

void Engine::RenderLoadingScreen() const
{
	const TextureID texLoadingScreen = mpRenderer->CreateTextureFromFile("LoadingScreen0.png");
	const XMMATRIX matTransformation = XMMatrixIdentity();
	const auto IABuffers = mBuiltinMeshes[EGeometry::QUAD].GetIABuffers();
	mpRenderer->BeginFrame();
	mpRenderer->BindRenderTarget(0);
	mpRenderer->SetShader(EShaders::UNLIT);
	mpRenderer->SetVertexBuffer(IABuffers.first);
	mpRenderer->SetIndexBuffer(IABuffers.second);
	mpRenderer->SetTexture("texDiffuseMap", texLoadingScreen);
	mpRenderer->SetConstant1f("isDiffuseMap", 1.0f);
	mpRenderer->SetConstant3f("diffuse", vec3(1.0f, 1, 1));
	mpRenderer->SetConstant4x4f("worldViewProj", matTransformation);
	mpRenderer->SetRasterizerState(EDefaultRasterizerState::CULL_NONE);
	mpRenderer->SetDepthStencilState(EDefaultDepthStencilState::DEPTH_STENCIL_DISABLED);
	mpRenderer->SetViewport(mpRenderer->WindowWidth(), mpRenderer->WindowHeight());
	mpRenderer->Apply();
	mpRenderer->DrawIndexed();
	mpRenderer->EndFrame();
	mpRenderer->UnbindRenderTargets();
}

const char* FrameStats::statNames[FrameStats::numStat] =
{
	"Objects: ",
	"Culled Objects: ",
	"Vertices: ",
	"Indices: ",
	"Draw Calls: ",
};

std::string CommaSeparatedNumber(const std::string& num)
{
	std::string _num = "";
	int i = 0;
	for (auto it = num.rbegin(); it != num.rend(); ++it)
	{
		if (i % 3 == 0 && i != 0)
		{
			_num += ",";
		}
		_num += *it;
		++i;
	}
	
	return std::string(_num.rbegin(), _num.rend());
}

void FrameStats::Render(TextRenderer* pTextRenderer, const vec2& screenPosition, const TextDrawDescription& drawDesc)
{
	TextDrawDescription _drawDesc(drawDesc);
	const float LINE_HEIGHT = 25.0f;
	constexpr size_t RENDER_ORDER[] = { 4, 2, 3, 0, 1 };

	pTextRenderer->RenderText(_drawDesc);
	for (size_t i = 0; i < FrameStats::numStat; ++i)
	{
		_drawDesc.screenPosition = vec2(screenPosition.x(), screenPosition.y() + i * LINE_HEIGHT);
		_drawDesc.text = FrameStats::statNames[RENDER_ORDER[i]] + CommaSeparatedNumber(std::to_string(FrameStats::stats[RENDER_ORDER[i]]));
		pTextRenderer->RenderText(_drawDesc);
	}
}
