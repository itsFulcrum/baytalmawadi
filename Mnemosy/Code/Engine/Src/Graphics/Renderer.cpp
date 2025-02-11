#include "Include/Graphics/Renderer.h"
#include "Include/MnemosyEngine.h"

#include "Include/Core/Window.h"
#include "Include/Core/Log.h"
#include "Include/Core/FileDirectories.h"
#include "Include/Systems/FolderTreeNode.h"

#include "Include/Systems/MeshRegistry.h"
#include "Include/Graphics/SceneSettings.h"
#include "Include/Graphics/Shader.h"
#include "Include/Graphics/ImageBasedLightingRenderer.h"
#include "Include/Graphics/Camera.h"
#include "Include/Graphics/Texture.h"
#include "Include/Graphics/Material.h"
#include "Include/Graphics/ModelData.h"
#include "Include/Graphics/RenderMesh.h"
#include "Include/Graphics/Cubemap.h"
#include "Include/Graphics/Skybox.h"
#include "Include/Graphics/Light.h"
#include "Include/Graphics/Scene.h"
#include "Include/Graphics/ThumbnailScene.h"


#include <json.hpp>
#include <filesystem>
#include <glad/glad.h>

namespace mnemosy::graphics
{
	// public

	void Renderer::Init() {
		// init members

		m_MSAA_FBO = 0;
		m_MSAA_RBO = 0;
		m_MSAA_renderTexture_ID = 0;

		m_standard_FBO = 0;
		m_standard_RBO = 0;
		m_standard_renderTexture_ID = 0;

		m_blitFBO = 0;
		m_blitRenderTexture_ID = 0;

		//m_clearColor = glm::vec3(0.0f, 0.0f, 0.0f);
		m_viewMatrix = glm::mat4(1.0f);
		m_projectionMatrix = glm::mat4(1.0f);
		
		m_pPbrShader = nullptr;
		m_pUnlitTexturesShader = nullptr;
		m_pLightShader = nullptr;
		m_pSkyboxShader = nullptr;

#ifdef MNEMOSY_RENDER_GIZMO
		m_pGizmoShader = nullptr;
#endif // MNEMOSY_RENDER_GIZMO

		m_msaaSamplesSettings = MSAA4X;
		m_msaaOff = false;

		// Thumbnails
		m_thumbnailResolution = ThumbnailResolution::MNSY_THUMBNAILRES_128;


		m_thumb_MSAA_Value = 16;
		m_thumb_MSAA_FBO = 0;
		m_thumb_MSAA_RBO = 0;
		m_thumb_MSAA_renderTexture_ID = 0;
		
		m_thumb_blitFBO = 0;
		m_thumb_blitTexture_ID = 0;

		m_renderMode = MNSY_RENDERMODE_SHADED;

		m_fileWatchTimeDelta = 0.0f;

		// load shaders

		MnemosyEngine& engine = MnemosyEngine::GetInstance();

		std::filesystem::path shaders = engine.GetFileDirectories().GetShadersPath();
		std::string shadersPath = shaders.generic_string() + "/";
		std::string pbrVert = shadersPath + "pbrVertex.vert";
		std::string pbrFrag = shadersPath + "pbrFragment.frag";
		std::string unlitFrag = shadersPath + "unlitTexView.frag";
		std::string unlitMatFrag = shadersPath + "unlitMaterial.frag";
		std::string unlitMatVert = shadersPath + "unlitMaterial.vert";


		std::string lightVert = shadersPath + "light.vert";
		std::string lightFrag = shadersPath + "light.frag";
		std::string skyboxVert = shadersPath + "skybox.vert";
		std::string skyboxFrag = shadersPath + "skybox.frag";


		MNEMOSY_DEBUG("Compiling Shaders");
		m_pPbrShader = new Shader(pbrVert.c_str(), pbrFrag.c_str());
		m_pUnlitTexturesShader = new Shader(pbrVert.c_str(), unlitFrag.c_str());

		m_pUnlitMaterialShader = new Shader(unlitMatVert.c_str(), unlitMatFrag.c_str());


		m_pLightShader = new Shader(lightVert.c_str(), lightFrag.c_str());
		m_pSkyboxShader = new Shader(skyboxVert.c_str(), skyboxFrag.c_str());


		unsigned int w = engine.GetWindow().GetWindowWidth();
		unsigned int h = engine.GetWindow().GetWindowHeight();
		CreateRenderingFramebuffer(w, h);
		CreateBlitFramebuffer(w, h);
		CreateThumbnailFramebuffers();

		m_shaderFileWatcher = core::FileWatcher();
		m_shaderSkyboxFileWatcher = core::FileWatcher();

		m_shaderUnlitFileWatcher = core::FileWatcher();

		// init FileWatcher
		{

			fs::path _includes = shaders / fs::path("includes");

			m_shaderUnlitFileWatcher.RegisterFile(fs::path(unlitMatVert));
			m_shaderUnlitFileWatcher.RegisterFile(fs::path(unlitMatFrag));

			m_shaderFileWatcher.RegisterFile(shaders / fs::path("pbrVertex.vert"));
			m_shaderFileWatcher.RegisterFile(fs::path(pbrFrag));
			m_shaderFileWatcher.RegisterFile(shaders / fs::path("unlitTexView.frag"));

			m_shaderFileWatcher.RegisterFile(_includes / fs::path("colorFunctions.glsl"));
			m_shaderFileWatcher.RegisterFile(_includes / fs::path("lighting.glsl"));
			m_shaderFileWatcher.RegisterFile(_includes / fs::path("mathFunctions.glsl"));
			m_shaderFileWatcher.RegisterFile(_includes / fs::path("pbrLightingTerms.glsl"));
			m_shaderFileWatcher.RegisterFile(_includes / fs::path("samplePbrMaps.glsl"));

			m_shaderSkyboxFileWatcher.RegisterFile(shaders / fs::path("skybox.vert"));
			m_shaderSkyboxFileWatcher.RegisterFile(shaders / fs::path("skybox.frag"));
		}

		LoadUserSettings();
	}

	void Renderer::Shutdown() {

		SaveUserSettings();

		delete m_pPbrShader;
		delete m_pLightShader;
		delete m_pSkyboxShader;
		delete m_pUnlitTexturesShader;
		delete m_pUnlitMaterialShader;
		m_pPbrShader = nullptr;
		m_pLightShader = nullptr;
		m_pSkyboxShader = nullptr;
		m_pUnlitTexturesShader = nullptr;

#ifdef MNEMOSY_RENDER_GIZMO
		delete m_pGizmoShader;
		m_pGizmoShader = nullptr;
#endif // MNEMOSY_RENDER_GIZMO


		// deleting MSAA Framebuffer stuff
		glDeleteRenderbuffers(1, &m_MSAA_RBO);
		glDeleteFramebuffers(1, &m_MSAA_FBO);
		glDeleteTextures(1, &m_MSAA_renderTexture_ID);

		glDeleteFramebuffers(1, &m_blitFBO);
		glDeleteTextures(1, &m_blitRenderTexture_ID);

		// deleting standard framebuffer stuff (no MSAA)
		glDeleteFramebuffers(1, &m_standard_FBO);
		glDeleteRenderbuffers(1, &m_standard_RBO);
		glDeleteTextures(1, &m_standard_renderTexture_ID);

		// Delete thumbnail textures and framebuffers
		glDeleteFramebuffers(1, &m_thumb_MSAA_FBO);
		glDeleteRenderbuffers(1, &m_thumb_MSAA_RBO);

		glDeleteTextures(1, &m_thumb_MSAA_renderTexture_ID);
		glDeleteFramebuffers(1, &m_thumb_blitFBO);
		glDeleteTextures(1, &m_thumb_blitTexture_ID);
	}

	// bind renderFrameBuffer
	void Renderer::BindFramebuffer() {

		if (m_msaaOff) {

			MNEMOSY_ASSERT(m_standard_FBO != 0, "Framebuffer has not be created yet");
			glBindFramebuffer(GL_FRAMEBUFFER, m_standard_FBO);			
			return;
		}

		MNEMOSY_ASSERT(m_MSAA_FBO != 0, "Framebuffer has not be created yet");
		glBindFramebuffer(GL_FRAMEBUFFER, m_MSAA_FBO);
	}

	void Renderer::UnbindFramebuffer() {

		glBindRenderbuffer(GL_RENDERBUFFER, 0);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}

	void Renderer::ResizeFramebuffer(unsigned int width, unsigned int height)
	{

		if (m_msaaOff) { // resizing standard framebuffer and texture
			
			glBindFramebuffer(GL_FRAMEBUFFER, m_standard_FBO);
			glBindTexture(GL_TEXTURE_2D, m_standard_renderTexture_ID);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr); // ====================
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

			glBindRenderbuffer(GL_RENDERBUFFER, m_standard_RBO);
			glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
			
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			return;
		}

		// Resizing multisampled (MSAA) framebuffers and textures
		glBindFramebuffer(GL_FRAMEBUFFER, m_MSAA_FBO);
		glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, m_MSAA_renderTexture_ID);

		int MSAA = GetMSAAIntValue();
		glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, MSAA, GL_RGB, width, height, GL_TRUE);

		glBindRenderbuffer(GL_RENDERBUFFER, m_MSAA_RBO);
		glRenderbufferStorageMultisample(GL_RENDERBUFFER, MSAA, GL_DEPTH24_STENCIL8, width, height);

		// resize intermediate blit framebuffer
		glBindFramebuffer(GL_FRAMEBUFFER,m_blitFBO);
		glBindTexture(GL_TEXTURE_2D, m_blitRenderTexture_ID);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	}

	unsigned int Renderer::GetRenderTextureId() {

		if (m_msaaOff) {
			return m_standard_renderTexture_ID;
		}
		return m_blitRenderTexture_ID;
	}

	
	void Renderer::SetPbrShaderBrdfLutUniforms()
	{
		m_pPbrShader->Use();
		MnemosyEngine::GetInstance().GetIblRenderer().BindBrdfLutTexture(10);
		m_pPbrShader->SetUniformInt("_brdfLUT", 10);
	}

	void Renderer::SetPbrShaderLightUniforms(Light& light)
	{

		//Light& light = MnemosyEngine::GetInstance().GetScene().GetLight();

		m_pPbrShader->Use();

		// depending on light type we pass light forward vector into light position and for point lights the actual position
		if (light.GetLightType() == LightType::DIRECTIONAL) {

			glm::vec3 lightForward = light.transform.GetForward();
			m_pPbrShader->SetUniformFloat3("_lightPositionWS", lightForward.x, lightForward.y, lightForward.z);
		}
		else if (light.GetLightType() == LightType::POINT)
		{
			glm::vec3 lightPosition = light.transform.GetPosition();
			m_pPbrShader->SetUniformFloat3("_lightPositionWS", lightPosition.x, lightPosition.y, lightPosition.z);
		}

		m_pPbrShader->SetUniformFloat("_lightStrength", light.strength);
		m_pPbrShader->SetUniformFloat3("_lightColor", light.color.r, light.color.g, light.color.b);
		m_pPbrShader->SetUniformInt("_lightType", light.GetLightTypeAsInt());
		m_pPbrShader->SetUniformFloat("_lightAttentuation", light.falloff);
	}

	// TODO: split this into two methods
	void Renderer::SetShaderSkyboxUniforms(SceneSettings& sceneSettings, Skybox& skybox) {
		
		m_pPbrShader->Use();
		
		bool skyboxHasTextures = skybox.HasCubemaps();// IsColorCubeAssigned();

		if (skyboxHasTextures) {

			skybox.GetIrradianceCube().Bind(8);
			m_pPbrShader->SetUniformInt("_irradianceMap", 8);

			skybox.GetPrefilterCube().Bind(9);
			m_pPbrShader->SetUniformInt("_prefilterMap", 9);

			int prefilterMaxMip = log2(skybox.GetPrefilterCube().GetResolution());
			m_pPbrShader->SetUniformInt("_prefilterMaxMip", prefilterMaxMip);

		
			m_pPbrShader->SetUniformFloat4("_skyboxColorValue", skybox.color.r, skybox.color.g, skybox.color.b, 1.0f);
		}
		else {
			m_pPbrShader->SetUniformFloat4("_skyboxColorValue", skybox.color.r, skybox.color.g, skybox.color.b, 0.0f);
			m_pSkyboxShader->SetUniformInt("_prefilterMaxMip", 0);
		}
		// set color and let shader know if skyboxes are bound


		m_pPbrShader->SetUniformFloat("_skyboxExposure", skybox.exposure);
		m_pPbrShader->SetUniformFloat("_skyboxRotation", sceneSettings.background_rotation);
		m_pPbrShader->SetUniformFloat("_postExposure", sceneSettings.globalExposure);

		m_pSkyboxShader->Use();

		if (skyboxHasTextures) {

			//skybox.GetColorCube().Bind(0);
			//m_pSkyboxShader->SetUniformInt("_skybox", 0);
			skybox.GetIrradianceCube().Bind(1);
			m_pSkyboxShader->SetUniformInt("_irradianceMap", 1);
			skybox.GetPrefilterCube().Bind(2);
			m_pSkyboxShader->SetUniformInt("_prefilterMap", 2);

			int prefilterMaxMip = log2(skybox.GetPrefilterCube().GetResolution());
			m_pSkyboxShader->SetUniformInt("_prefilterMaxMip", prefilterMaxMip);

			m_pSkyboxShader->SetUniformFloat4("_skyboxColorValue",skybox.color.r, skybox.color.g, skybox.color.b, 1.0f);
		}
		else {

			m_pSkyboxShader->SetUniformFloat4("_skyboxColorValue",skybox.color.r, skybox.color.g, skybox.color.b, 0.0f);

			m_pSkyboxShader->SetUniformInt("_prefilterMaxMip", 0);
		}


		m_pSkyboxShader->SetUniformFloat("_postExposure", sceneSettings.globalExposure);
		m_pSkyboxShader->SetUniformFloat("_exposure", skybox.exposure);

		m_pSkyboxShader->SetUniformFloat("_rotation", sceneSettings.background_rotation);
		m_pSkyboxShader->SetUniformFloat("_blurRadius", sceneSettings.background_blurRadius);
		m_pSkyboxShader->SetUniformFloat3("_backgroundColor", sceneSettings.background_color_r, sceneSettings.background_color_g, sceneSettings.background_color_b);
		m_pSkyboxShader->SetUniformFloat("_gradientOpacity", sceneSettings.background_gradientOpacity);
		m_pSkyboxShader->SetUniformFloat("_opacity", sceneSettings.background_opacity);
		//m_pSkyboxShader->SetUniformInt("_blurSteps", sceneSettings.background_blurSteps);

	}

	void Renderer::SetProjectionMatrix(const glm::mat4& projectionMatrix)
	{
		m_projectionMatrix = projectionMatrix;
	}

	void Renderer::SetViewMatrix(const glm::mat4& viewMatrix)
	{
		m_viewMatrix = viewMatrix;
	}

	//void Renderer::SetClearColor(float r, float g, float b)
	//{
	//	m_clearColor.r = r;
	//	m_clearColor.g = g;
	//	m_clearColor.b = b;
	//}

	void Renderer::ClearFrame()
	{
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		//glEnable(GL_DEPTH_TEST);
	}

	void Renderer::StartFrame(unsigned int width, unsigned int height)
	{
		int width_ = (int)width;
		int height_ = (int)height;

		glViewport(0, 0, width_, height_);
		ResizeFramebuffer(width_ , height_);

		//glViewport(0, 0, width_, height_);
		//glBindFramebuffer(GL_FRAMEBUFFER, m_frameBufferObject);
		
		BindFramebuffer();
		ClearFrame();
	}

	void Renderer::EndFrame(unsigned int width, unsigned int height) {

		if (!m_msaaOff) {

			// resolve multisampled buffer into intermediate blit FBO
			// we dont have to bind m_frameBufferObject here again because it is already bound at framestart.
			//glBindFramebuffer(GL_READ_FRAMEBUFFER, m_frameBufferObject);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_blitFBO);
			glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		}		
	
		UnbindFramebuffer();
	}

	void Renderer::RenderMeshes(RenderMesh& renderMesh, Shader* shader) {

		glm::mat4 modelMatrix = renderMesh.transform.GetTransformMatrix();

		shader->Use();
		shader->SetUniformMatrix4("_modelMatrix", modelMatrix);
		shader->SetUniformMatrix4("_normalMatrix", renderMesh.transform.GetNormalMatrix(modelMatrix));
		shader->SetUniformMatrix4("_projectionMatrix", m_projectionMatrix);
		shader->SetUniformMatrix4("_viewMatrix", m_viewMatrix);
		


		for (unsigned int i = 0; i < renderMesh.GetModelData().meshes.size(); i++) {
			glBindVertexArray(renderMesh.GetModelData().meshes[i].vertexArrayObject);
			glDrawElements(GL_TRIANGLES, (GLsizei)renderMesh.GetModelData().meshes[i].indecies.size(), GL_UNSIGNED_INT, 0);
		}
		glBindVertexArray(0);

	}

	void Renderer::RenderGizmo(RenderMesh& renderMesh)
	{
#ifdef MNEMOSY_RENDER_GIZMO


		m_pGizmoShader->Use();
		glm::mat4 modelMatrix = renderMesh.transform.GetTransformMatrix();
		m_pGizmoShader->SetUniformMatrix4("_modelMatrix", modelMatrix);
		m_pGizmoShader->SetUniformMatrix4("_normalMatrix", renderMesh.transform.GetNormalMatrix(modelMatrix));
		m_pGizmoShader->SetUniformMatrix4("_viewMatrix", glm::mat4(glm::mat3(m_viewMatrix)));
		m_pGizmoShader->SetUniformMatrix4("_projectionMatrix", m_projectionMatrix);

		for (unsigned int i = 0; i < renderMesh.GetModelData().meshes.size(); i++)
		{
			glBindVertexArray(renderMesh.GetModelData().meshes[i].vertexArrayObject);
			glDrawElements(GL_TRIANGLES, (GLsizei)renderMesh.GetModelData().meshes[i].indecies.size(), GL_UNSIGNED_INT, 0);
		}
		glBindVertexArray(0);
#endif // MNEMOSY_RENDER_GIZMO
	}

	void Renderer::RenderLightMesh(Light& light)
	{
		m_pLightShader->Use();

		m_pLightShader->SetUniformFloat("_lightStrength", light.strength);
		m_pLightShader->SetUniformFloat3("_lightColor", light.color.r, light.color.g, light.color.b);
		//m_pPbrShader->SetUniformInt("_lightType", light.GetLightTypeAsInt());
		//m_pPbrShader->SetUniformFloat("_lightAttentuation", light.falloff);

		m_pLightShader->SetUniformMatrix4("_modelMatrix", light.transform.GetTransformMatrix());
		m_pLightShader->SetUniformMatrix4("_viewMatrix", m_viewMatrix);
		m_pLightShader->SetUniformMatrix4("_projectionMatrix", m_projectionMatrix);

		for (unsigned int i = 0; i < light.GetModelData().meshes.size(); i++)
		{
			glBindVertexArray(light.GetModelData().meshes[i].vertexArrayObject);
			glDrawElements(GL_TRIANGLES, (GLsizei)light.GetModelData().meshes[i].indecies.size(), GL_UNSIGNED_INT, 0);
		}
		glBindVertexArray(0);

	}

	void Renderer::RenderSkybox(Skybox& skybox)
	{
		// consider using a mesh that has faces point inwards to make this call obsolete
		glCullFace(GL_BACK);
		glDepthFunc(GL_LEQUAL);

		m_pSkyboxShader->Use();

		glm::mat4 skyboxViewMatrix = glm::mat4(glm::mat3(m_viewMatrix));
		m_pSkyboxShader->SetUniformMatrix4("_viewMatrix", skyboxViewMatrix);
		m_pSkyboxShader->SetUniformMatrix4("_projectionMatrix", m_projectionMatrix);


		ModelData& skyboxModel = MnemosyEngine::GetInstance().GetMeshRegistry().GetSkyboxRenderMesh();
		for (unsigned int i = 0; i < skyboxModel.meshes.size(); i++)
		{
			glBindVertexArray(skyboxModel.meshes[i].vertexArrayObject);
			glDrawElements(GL_TRIANGLES, (GLsizei)skyboxModel.meshes[i].indecies.size(), GL_UNSIGNED_INT, 0);
		}

		glBindVertexArray(0);

		glDepthFunc(GL_LESS);
		glCullFace(GL_FRONT);
	}

	void Renderer::RenderScene(Scene& scene, systems::LibEntryType materialType) {

		MNEMOSY_ASSERT(materialType != systems::LibEntryType::MNSY_ENTRY_TYPE_SKYBOX, "Renderer needs to know the material type to use for rendering not the skybox type");

		unsigned int width = MnemosyEngine::GetInstance().GetWindow().GetViewportWidth();
		unsigned int height = MnemosyEngine::GetInstance().GetWindow().GetViewportHeight();
		
		scene.GetCamera().SetScreenSize(width,height);

		SetViewMatrix(scene.GetCamera().GetViewMatrix());
		SetProjectionMatrix(scene.GetCamera().GetProjectionMatrix());

		StartFrame(width, height);

		glm::vec3 cameraPosition = scene.GetCamera().transform.GetPosition();
		
		graphics::Shader* shaderToUse = m_pPbrShader;

		if (materialType == systems::LibEntryType::MNSY_ENTRY_TYPE_PBRMAT) {


			if (m_renderMode != MNSY_RENDERMODE_SHADED) {

				shaderToUse = m_pUnlitTexturesShader;

				m_pUnlitTexturesShader->Use();
				m_pUnlitTexturesShader->SetUniformInt("_mode", (int)m_renderMode);

				scene.GetPbrMaterial().setMaterialUniforms(*m_pUnlitTexturesShader);
			}
			else {		
				shaderToUse = m_pPbrShader;
				scene.GetPbrMaterial().setMaterialUniforms(*m_pPbrShader);
			}
		}
		else if (materialType == systems::LibEntryType::MNSY_ENTRY_TYPE_UNLITMAT) {
		
			shaderToUse = m_pUnlitMaterialShader;

			scene.GetUnlitMaterial()->SetUniforms(m_pUnlitMaterialShader);
			
		}

		// set common uniforms
		shaderToUse->Use();
		shaderToUse->SetUniformFloat3("_cameraPositionWS", cameraPosition.x, cameraPosition.y, cameraPosition.z);
		shaderToUse->SetUniformInt("_pixelWidth", width);
		shaderToUse->SetUniformInt("_pixelHeight", height);

		RenderMeshes(scene.GetMesh(), shaderToUse);
		RenderLightMesh(scene.GetLight());
		RenderSkybox(scene.GetSkybox());

		EndFrame(width,height);
	}


	
	void Renderer::RenderThumbnail_PbrMaterial(PbrMaterial& activeMaterial) {

		unsigned int thumbRes = GetThumbnailResolutionValue(m_thumbnailResolution);
		ThumbnailScene& thumbScene = MnemosyEngine::GetInstance().GetThumbnailScene();
		
		RenderModes userRenderMode = m_renderMode;
		m_renderMode = MNSY_RENDERMODE_SHADED;


		// Setup Shaders with thumbnail Scene settings
		SetPbrShaderLightUniforms(thumbScene.GetLight());
		SetShaderSkyboxUniforms(thumbScene.GetSceneSettings(),thumbScene.GetSkybox());

		thumbScene.GetCamera().SetScreenSize(thumbRes, thumbRes); // we should really only need to do this once..

		m_projectionMatrix = thumbScene.GetCamera().GetProjectionMatrix();
		m_viewMatrix = thumbScene.GetCamera().GetViewMatrix();

		// Start Frame
		glViewport(0, 0, thumbRes, thumbRes);
		
		// resize thumbnail render texture
		glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, m_thumb_MSAA_renderTexture_ID);
		glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, m_thumb_MSAA_Value, GL_RGB, thumbRes, thumbRes, GL_TRUE);

		// rezie thumbnail renderbuffer
		glBindRenderbuffer(GL_RENDERBUFFER, m_thumb_MSAA_RBO);
		glRenderbufferStorageMultisample(GL_RENDERBUFFER, m_thumb_MSAA_Value, GL_DEPTH24_STENCIL8, thumbRes, thumbRes);

		// resize thumbnail blit fbo
		glBindFramebuffer(GL_FRAMEBUFFER, m_thumb_blitFBO);

		glBindTexture(GL_TEXTURE_2D, m_thumb_blitTexture_ID);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, thumbRes, thumbRes, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

		// ====
		glBindFramebuffer(GL_FRAMEBUFFER, m_thumb_MSAA_FBO);

		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		m_pPbrShader->Use();

		glm::vec3 cameraPosition = thumbScene.GetCamera().transform.GetPosition();
		m_pPbrShader->SetUniformFloat3("_cameraPositionWS", cameraPosition.x, cameraPosition.y, cameraPosition.z);

		activeMaterial.setMaterialUniforms(*m_pPbrShader);


		RenderMeshes(thumbScene.GetMesh(), m_pPbrShader);

		// render skybox normally
		RenderSkybox(thumbScene.GetSkybox());

		// End Frame
		// blit msaa framebuffer to normal framebuffer
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER,m_thumb_blitFBO);
		glBlitFramebuffer(0, 0, thumbRes, thumbRes, 0, 0, thumbRes, thumbRes, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		// unbind frambuffers
		glBindRenderbuffer(GL_RENDERBUFFER, 0);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);


		// Restore user pbr shader settings
		m_renderMode = userRenderMode;
		Scene& scene = MnemosyEngine::GetInstance().GetScene();
		SetPbrShaderLightUniforms(scene.GetLight());
		SetShaderSkyboxUniforms(scene.userSceneSettings, scene.GetSkybox());
	}

	void Renderer::RenderThumbnail_UnlitMaterial(UnlitMaterial* unlitMaterial)
	{
		unsigned int thumbRes = GetThumbnailResolutionValue(m_thumbnailResolution);
		ThumbnailScene& thumbScene = MnemosyEngine::GetInstance().GetThumbnailScene();

		// Setup Shaders with thumbnail Scene settings		

		// we need this bc we want to render skybox in the background if texture has alpha test
		SetShaderSkyboxUniforms(thumbScene.GetSceneSettings(), thumbScene.GetSkybox());

		thumbScene.GetCamera().SetScreenSize(thumbRes, thumbRes);


		// Start Frame And setup framebuffers
		glViewport(0, 0, thumbRes, thumbRes);

		// resize thumbnail render texture
		glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, m_thumb_MSAA_renderTexture_ID);
		glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, m_thumb_MSAA_Value, GL_RGB, thumbRes, thumbRes, GL_TRUE);

		// resize thumbnail renderbuffer
		glBindRenderbuffer(GL_RENDERBUFFER, m_thumb_MSAA_RBO);
		glRenderbufferStorageMultisample(GL_RENDERBUFFER, m_thumb_MSAA_Value, GL_DEPTH24_STENCIL8, thumbRes, thumbRes);

		// resize thumbnail blit fbo
		glBindFramebuffer(GL_FRAMEBUFFER, m_thumb_blitFBO);

		glBindTexture(GL_TEXTURE_2D, m_thumb_blitTexture_ID);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, thumbRes, thumbRes, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

		// ====
		glBindFramebuffer(GL_FRAMEBUFFER, m_thumb_MSAA_FBO);

		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		// setup uniforms for the shader
		m_pUnlitMaterialShader->Use();

		glm::vec3 cameraPosition = thumbScene.GetCamera().transform.GetPosition();
		
		unlitMaterial->SetUniforms(m_pUnlitMaterialShader);

		m_pUnlitMaterialShader->SetUniformFloat3("_cameraPositionWS", cameraPosition.x, cameraPosition.y, cameraPosition.z);

		// we use this matrix here for model, projection and view matrices because we want 
		// to render a quad in screen space without any camera or model projections and this way we dont need a sperate shader
		glm::mat4 mat = glm::mat4(1);
		m_pUnlitMaterialShader->SetUniformMatrix4("_modelMatrix", mat);
		m_pUnlitMaterialShader->SetUniformMatrix4("_normalMatrix", glm::transpose(glm::inverse(mat)));

		m_pUnlitMaterialShader->SetUniformMatrix4("_projectionMatrix", mat);
		m_pUnlitMaterialShader->SetUniformMatrix4("_viewMatrix",mat);

		m_pUnlitMaterialShader->SetUniformInt("_pixelWidth", thumbRes);
		m_pUnlitMaterialShader->SetUniformInt("_pixelHeight", thumbRes);


		/// calculate uv tiling and offset based on width and height of the texture.
		// this is to ensure non square images are correctly visible
		// x 1024  y 512

		if (unlitMaterial->TextureIsAssigned()) {


			float x = (float)unlitMaterial->GetTexture().GetWidth(); // e.g. 1024
			float y = (float)unlitMaterial->GetTexture().GetHeight(); // e.g. 512

			float uv_tile_x = 1.0f;
			float uv_tile_y = 1.0f;
			float uv_offset_x = 0.0f;
			float uv_offset_y = 0.0f;

			if (x != y) {

				float aspect_y = x / y;  // 1024 / 512 = 2
				float aspect_x = y / x;  // 512 / 1024 = 0.5f

				if (aspect_y > 1.00000000f) {
					uv_tile_y = aspect_y;
					uv_offset_y = aspect_x;
				}

				if (aspect_x > 1.00000000f) {
					uv_tile_x = aspect_x;
					uv_offset_x = aspect_y;
				}

			}
			m_pUnlitMaterialShader->SetUniformFloat2("_uvTiling", uv_tile_x, uv_tile_y);
			m_pUnlitMaterialShader->SetUniformFloat2("_uvOffset", uv_offset_x, uv_offset_y);
		}




		// Draw call with screen quad
		glBindVertexArray(MnemosyEngine::GetInstance().GetMeshRegistry().GetScreenQuadVAO());
		glDrawArrays(GL_TRIANGLES, 0, 6); // 6 vertecies
		glBindVertexArray(0);

		// render skybox normally for the background
		RenderSkybox(thumbScene.GetSkybox());

		// End Frame
		// blit msaa framebuffer to normal framebuffer
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_thumb_blitFBO);
		glBlitFramebuffer(0, 0, thumbRes, thumbRes, 0, 0, thumbRes, thumbRes, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		// unbind frambuffers
		glBindRenderbuffer(GL_RENDERBUFFER, 0);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);


		// Restore user pbr shader settings
		Scene& scene = MnemosyEngine::GetInstance().GetScene();
		//SetPbrShaderLightUniforms(scene.GetLight());
		SetShaderSkyboxUniforms(scene.userSceneSettings, scene.GetSkybox());
	}

	void Renderer::RenderThumbnail_SkyboxMaterial(Skybox& skyboxMaterial)
	{

		// try to maybe render glossy ball with skybox in the background

		
		unsigned int thumbRes = GetThumbnailResolutionValue(m_thumbnailResolution);
		ThumbnailScene& thumbScene = MnemosyEngine::GetInstance().GetThumbnailScene();


		// Start Frame And setup framebuffers
		glViewport(0, 0, thumbRes, thumbRes);

		// resize thumbnail render texture
		glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, m_thumb_MSAA_renderTexture_ID);
		glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, m_thumb_MSAA_Value, GL_RGB, thumbRes, thumbRes, GL_TRUE);

		// resize thumbnail renderbuffer
		glBindRenderbuffer(GL_RENDERBUFFER, m_thumb_MSAA_RBO);
		glRenderbufferStorageMultisample(GL_RENDERBUFFER, m_thumb_MSAA_Value, GL_DEPTH24_STENCIL8, thumbRes, thumbRes);

		// resize thumbnail blit fbo
		glBindFramebuffer(GL_FRAMEBUFFER, m_thumb_blitFBO);

		glBindTexture(GL_TEXTURE_2D, m_thumb_blitTexture_ID);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, thumbRes, thumbRes, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

		// ====
		glBindFramebuffer(GL_FRAMEBUFFER, m_thumb_MSAA_FBO);

		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);


		m_pSkyboxShader->Use();
		if (skyboxMaterial.HasCubemaps()) {
			//skyboxMaterial.GetColorCube().Bind(0);
			//m_pSkyboxShader->SetUniformInt("_skybox", 0);
			skyboxMaterial.GetIrradianceCube().Bind(1);
			m_pSkyboxShader->SetUniformInt("_irradianceMap", 1);
			skyboxMaterial.GetPrefilterCube().Bind(2);
			m_pSkyboxShader->SetUniformInt("_prefilterMap", 2);
			
			int prefilterMaxMip = 0; // since we dont use blurring for thumbnails we can just set this to 0  log2(skyboxMaterial.GetPrefilterCube().GetResolution());
			m_pSkyboxShader->SetUniformInt("_prefilterMaxMip", prefilterMaxMip);


			m_pSkyboxShader->SetUniformFloat4("_skyboxColorValue", skyboxMaterial.color.r, skyboxMaterial.color.g, skyboxMaterial.color.b, 1.0f);
		}
		else {
		
			m_pSkyboxShader->SetUniformFloat4("_skyboxColorValue", skyboxMaterial.color.r, skyboxMaterial.color.g, skyboxMaterial.color.b, 0.0f);

		}
		
		m_pSkyboxShader->SetUniformFloat("_postExposure", thumbScene.GetSceneSettings().globalExposure);

		m_pSkyboxShader->SetUniformFloat("_exposure", skyboxMaterial.exposure);
		m_pSkyboxShader->SetUniformFloat("_rotation", 0.0f);
		m_pSkyboxShader->SetUniformFloat("_blurRadius", 0.0f);
		m_pSkyboxShader->SetUniformFloat3("_backgroundColor", 0.0f, 0.0f, 0.0f);
		m_pSkyboxShader->SetUniformFloat("_gradientOpacity", 0.0f);
		m_pSkyboxShader->SetUniformFloat("_opacity", 1.0f);
		m_pSkyboxShader->SetUniformInt("_blurSteps",0);

		
		// we create a custom projection matrix here with higher field of view to render the skybox
		//thumbScene.GetCamera().SetScreenSize(thumbRes, thumbRes);
		glm::mat4 skyboxViewMatrix = glm::mat4(glm::mat3(thumbScene.GetCamera().GetViewMatrix()));
		glm::mat4 customProjection = glm::perspective(glm::radians(120.0f), float(thumbRes) / float(thumbRes), 0.1f, 500.0f);

		m_pSkyboxShader->SetUniformMatrix4("_viewMatrix", skyboxViewMatrix);
		m_pSkyboxShader->SetUniformMatrix4("_projectionMatrix", customProjection);


		// render skybox custom here
		glCullFace(GL_BACK);
		glDepthFunc(GL_LEQUAL);

		ModelData& skyboxModel = MnemosyEngine::GetInstance().GetMeshRegistry().GetSkyboxRenderMesh();
		for (unsigned int i = 0; i < skyboxModel.meshes.size(); i++)
		{
			glBindVertexArray(skyboxModel.meshes[i].vertexArrayObject);
			glDrawElements(GL_TRIANGLES, (GLsizei)skyboxModel.meshes[i].indecies.size(), GL_UNSIGNED_INT, 0);
		}

		glBindVertexArray(0);

		glDepthFunc(GL_LESS);
		glCullFace(GL_FRONT);

		// End Frame
		// blit msaa framebuffer to normal framebuffer
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_thumb_blitFBO);
		glBlitFramebuffer(0, 0, thumbRes, thumbRes, 0, 0, thumbRes, thumbRes, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		// unbind frambuffers
		glBindRenderbuffer(GL_RENDERBUFFER, 0);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);


		// Restore user pbr shader settings
		Scene& scene = MnemosyEngine::GetInstance().GetScene();
		//SetPbrShaderLightUniforms(scene.GetLight());
		SetShaderSkyboxUniforms(scene.userSceneSettings, scene.GetSkybox());
	}

	void Renderer::SetMSAASamples(const MSAAsamples& samples)
	{
		m_msaaOff = false;
		if (samples == MSAAOFF) {
			m_msaaOff = true;
		}

		m_msaaSamplesSettings = samples;
	}

	unsigned int Renderer::GetThumbnailResolutionValue(ThumbnailResolution thumbnailResolution) {
		switch (thumbnailResolution)
		{
		case mnemosy::graphics::MNSY_THUMBNAILRES_64:	return 64;	break;
		case mnemosy::graphics::MNSY_THUMBNAILRES_128:	return 128;	break;
		case mnemosy::graphics::MNSY_THUMBNAILRES_256:	return 256;	break;
		case mnemosy::graphics::MNSY_THUMBNAILRES_512:	return 512; break;
		default: return 128;
			break;
		}

		return 0;
	}

	// private
	void Renderer::CreateRenderingFramebuffer(unsigned int width, unsigned int height) {
		// MSAA FRAMEBUFFERS
		// Generate MSAA Framebuffer
		glGenFramebuffers(1, &m_MSAA_FBO);
		glBindFramebuffer(GL_FRAMEBUFFER, m_MSAA_FBO);
		// Generate MSAA Render Texture
		glGenTextures(1, &m_MSAA_renderTexture_ID);
		glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, m_MSAA_renderTexture_ID);

		int MSAA = GetMSAAIntValue();
		glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, MSAA , GL_RGB, width, height, GL_TRUE);
		glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
		// bind MSAA render texture to MSAA framebuffer
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, m_MSAA_renderTexture_ID, 0);
		// dont work with multisampling
		//glTexParameteri(GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		//glTexParameteri(GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		// Generate MSAA Renderbuffer
		glGenRenderbuffers(1, &m_MSAA_RBO);
		glBindRenderbuffer(GL_RENDERBUFFER, m_MSAA_RBO);
		glRenderbufferStorageMultisample(GL_RENDERBUFFER,MSAA, GL_DEPTH24_STENCIL8, width, height);
		glBindRenderbuffer(GL_RENDERBUFFER, 0);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_MSAA_RBO);

		MNEMOSY_ASSERT(glad_glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "Faild to complete MSAA framebuffer");

		// STANDARD FRAMEBUFFERS
		// Generate standart Framebuffer (no MSAA)
		glGenFramebuffers(1, &m_standard_FBO);
		glBindFramebuffer(GL_FRAMEBUFFER, m_standard_FBO);
		// Generate MSAA Render Texture
		glGenTextures(1, &m_standard_renderTexture_ID);
		glBindTexture(GL_TEXTURE_2D, m_standard_renderTexture_ID);
		glTexImage2D(GL_TEXTURE_2D,0, GL_RGB, width, height,0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
		glBindTexture(GL_TEXTURE_2D, 0);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		// bind standard render texture to standard framebuffer
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_standard_renderTexture_ID, 0);
	
		// Generate MSAA Renderbuffer
		glGenRenderbuffers(1, &m_standard_RBO);
		glBindRenderbuffer(GL_RENDERBUFFER, m_standard_RBO);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
		glBindRenderbuffer(GL_RENDERBUFFER, 0);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_standard_RBO);

		MNEMOSY_ASSERT(glad_glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "Faild to complete standard framebuffer");

		// unbind everything		
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
		glBindTexture(GL_TEXTURE_2D, 0);
		glBindRenderbuffer(GL_RENDERBUFFER, 0);

		MNEMOSY_DEBUG("Renderer: Framebuffer created");

	}

	void Renderer::CreateBlitFramebuffer(unsigned int width, unsigned int height)
	{
		glGenFramebuffers(1, &m_blitFBO);
		glBindFramebuffer(GL_FRAMEBUFFER, m_blitFBO);


		glGenTextures(1, &m_blitRenderTexture_ID);
		glBindTexture(GL_TEXTURE_2D,m_blitRenderTexture_ID);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height,0,GL_RGB,GL_UNSIGNED_BYTE,nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_blitRenderTexture_ID, 0);

		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glBindTexture(GL_TEXTURE_2D, 0);
	}

	void Renderer::CreateThumbnailFramebuffers() {

		const int thumbnailMSAA = m_thumb_MSAA_Value;

		int thumbnailRes = GetThumbnailResolutionValue(m_thumbnailResolution);
		
		// Gen msaa framebuffer
		glGenFramebuffers(1, &m_thumb_MSAA_FBO);
		glBindFramebuffer(GL_FRAMEBUFFER, m_thumb_MSAA_FBO);

		// Generate msaa render texture
		glGenTextures(1, &m_thumb_MSAA_renderTexture_ID);
		glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, m_thumb_MSAA_renderTexture_ID);
		glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, thumbnailMSAA, GL_RGB, thumbnailRes, thumbnailRes, GL_TRUE);
		glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);

		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, m_thumb_MSAA_renderTexture_ID, 0);

		glGenRenderbuffers(1, &m_thumb_MSAA_RBO);
		glBindRenderbuffer(GL_RENDERBUFFER, m_thumb_MSAA_RBO);
		glRenderbufferStorageMultisample(GL_RENDERBUFFER, thumbnailMSAA, GL_DEPTH24_STENCIL8, thumbnailRes, thumbnailRes);
		glBindRenderbuffer(GL_RENDERBUFFER, 0);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_thumb_MSAA_RBO);

		// check if frambuffer complete
		MNEMOSY_ASSERT(glad_glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "Faild to complete framebuffer");

		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0); // might be redundant call
		glBindRenderbuffer(GL_RENDERBUFFER, 0); // might be redundant call

		// generate standart texture and framebuffers for blitting msaa render output to
		glGenFramebuffers(1, &m_thumb_blitFBO);
		glBindFramebuffer(GL_FRAMEBUFFER, m_thumb_blitFBO);

		glGenTextures(1, &m_thumb_blitTexture_ID);
		glBindTexture(GL_TEXTURE_2D, m_thumb_blitTexture_ID);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, thumbnailRes, thumbnailRes, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_thumb_blitTexture_ID, 0);
		
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glBindTexture(GL_TEXTURE_2D, 0);
	}

	int Renderer::GetMSAAIntValue()
	{
		switch (m_msaaSamplesSettings) {

		case (MSAAOFF):
			return 0;
			break;
		case (MSAA2X):
			return 2;
			break;
		case (MSAA4X):
			return 4;
			break;
		case (MSAA8X):
			return 8;
			break;
		case (MSAA16X):
			return 16;
			break;
		}

		return 4;
	}

	void Renderer::SetRenderMode(RenderModes mode) {

		if (m_renderMode == mode)
			return;

		m_renderMode = mode;
	}

	void Renderer::HotReloadPbrShader(float deltaSeconds) {


		float waitTime = 5.0f;
#ifdef MNEMOSY_CONFIG_DEBUG
		waitTime = 0.5f;
#endif // MNEMOSY_CONFIG_DEBUG


		// only check every couple of seconds
		m_fileWatchTimeDelta += deltaSeconds;
		if (m_fileWatchTimeDelta >= waitTime) {

			m_fileWatchTimeDelta = 0.0f;
		}
		else {
			return;
		}


		// check unlit shader



		if (m_shaderUnlitFileWatcher.DidAnyFileChange()) {

			MNEMOSY_INFO("Recompiling Unlit Material Shader.");
			
			std::filesystem::path shaders = MnemosyEngine::GetInstance().GetFileDirectories().GetShadersPath();

			fs::path vertPath = shaders / fs::path("unlitMaterial.vert");
			fs::path fragPath = shaders / fs::path("unlitMaterial.frag");

			bool success = m_pUnlitMaterialShader->CreateShaderProgram(vertPath.generic_string().c_str(),fragPath.generic_string().c_str());
			if (!success) {
			
				MNEMOSY_WARN("Shader Recompilation failed. Switching to fallback shader.");

				fs::path fallbackVertPath = shaders / fs::path("fallback.vert");
				fs::path fallbackFragPath = shaders / fs::path("fallback.frag");
				m_pUnlitMaterialShader->CreateShaderProgram(fallbackVertPath.generic_string().c_str(), fallbackFragPath.generic_string().c_str());

			}

		}


		// checking pbr shader
		if (m_shaderFileWatcher.DidAnyFileChange()) {
			
			MNEMOSY_INFO("Renderer::HotRealoadPbrShader: Recompiling pbr shader.");


			std::filesystem::path shaders = MnemosyEngine::GetInstance().GetFileDirectories().GetShadersPath();
			fs::path vertPath = shaders / fs::path("pbrVertex.vert");
			fs::path fragPath = shaders / fs::path("pbrFragment.frag");
			fs::path unlitFragPath = shaders / fs::path("unlitTexView.frag");

			bool success = m_pPbrShader->CreateShaderProgram(vertPath.generic_string().c_str(), fragPath.generic_string().c_str());
			success = m_pUnlitTexturesShader->CreateShaderProgram(vertPath.generic_string().c_str(), unlitFragPath.generic_string().c_str());
			if (success) {

				// reassign uniforms
				SetPbrShaderBrdfLutUniforms();
				SetPbrShaderLightUniforms(MnemosyEngine::GetInstance().GetScene().GetLight());
				SetShaderSkyboxUniforms(MnemosyEngine::GetInstance().GetScene().userSceneSettings,MnemosyEngine::GetInstance().GetScene().GetSkybox());
			}
			else {
				// compilation failed assign fallback shader;
				MNEMOSY_WARN("Shader Recompilation failed. Switching to fallback shader.");

				fs::path fallbackVertPath = shaders / fs::path("fallback.vert");
				fs::path fallbackFragPath = shaders / fs::path("fallback.frag");

				m_pPbrShader->CreateShaderProgram(fallbackVertPath.generic_string().c_str(), fallbackFragPath.generic_string().c_str());
				m_pUnlitTexturesShader->CreateShaderProgram(fallbackVertPath.generic_string().c_str(), fallbackFragPath.generic_string().c_str());
			}
		}


		if (m_shaderSkyboxFileWatcher.DidAnyFileChange()) {


			MNEMOSY_INFO("Renderer::HotRealoadPbrShader: Recompiling skybox shader.");

			std::filesystem::path shaders = MnemosyEngine::GetInstance().GetFileDirectories().GetShadersPath();

			fs::path vertPath = shaders / fs::path("skybox.vert");
			fs::path fragPath = shaders / fs::path("skybox.frag");


			bool success = m_pSkyboxShader->CreateShaderProgram(vertPath.generic_string().c_str(), fragPath.generic_string().c_str());
			if (success) {



				SetShaderSkyboxUniforms(MnemosyEngine::GetInstance().GetScene().userSceneSettings, MnemosyEngine::GetInstance().GetScene().GetSkybox());
			}
			else {
				MNEMOSY_WARN("Renderer::HotRealoadPbrShader: Compilationfailed. Switching to fallback shader.");


				fs::path fallbackVertPath = shaders / fs::path("skybox_fallback.vert");
				fs::path fallbackFragPath = shaders / fs::path("skybox_fallback.frag");


				m_pSkyboxShader->CreateShaderProgram(fallbackVertPath.generic_string().c_str(), fallbackFragPath.generic_string().c_str());
			
			}



		}


	}

	void Renderer::LoadUserSettings() {

		std::filesystem::path renderSettingsFilePath = MnemosyEngine::GetInstance().GetFileDirectories().GetUserSettingsPath() / std::filesystem::path("renderSettings.mnsydata");

		bool success;

		flcrm::JsonSettings renderSettings;
		renderSettings.FileOpen(success,renderSettingsFilePath,"Mnemosy Settings File","Stores Render Settings");
		if (!success) {
			MNEMOSY_ERROR("Rederer: Faild to open user settings file: {}", renderSettings.ErrorStringLastGet());
		}



		int Msaa = renderSettings.ReadInt(success,"renderSettings_MSAA", 4, true);
		int thumbnailRes = renderSettings.ReadInt(success, "renderSettings_ThumbnailResolution", 256, true);

		renderSettings.FilePrettyPrintSet(true);
		renderSettings.FileClose(success,renderSettingsFilePath);


		// apply msaa
		if (Msaa == 0) {
			SetMSAASamples(graphics::MSAAsamples::MSAAOFF);
		}
		else if (Msaa == 2) {
			SetMSAASamples(graphics::MSAAsamples::MSAA2X);
		}
		else if (Msaa == 4) {
			SetMSAASamples(graphics::MSAAsamples::MSAA4X);
		}
		else if (Msaa == 8) {
			SetMSAASamples(graphics::MSAAsamples::MSAA8X);
		}
		else if (Msaa == 16) {
			SetMSAASamples(graphics::MSAAsamples::MSAA16X);
		}
		else {
			SetMSAASamples(graphics::MSAAsamples::MSAA4X);
		}

		// apply thumbnailResolution


		for (int thumbResEnum = 0; thumbResEnum < (int)ThumbnailResolution::MNSY_THUMBNAILRES_COUNT; thumbResEnum++) {

			if (thumbnailRes == GetThumbnailResolutionValue((ThumbnailResolution)thumbResEnum)) {

				SetThumbnailResolution((ThumbnailResolution)thumbResEnum);
				break;
			}
		}

	}

	void Renderer::SaveUserSettings() {

		std::filesystem::path renderSettingsFilePath = MnemosyEngine::GetInstance().GetFileDirectories().GetUserSettingsPath() / std::filesystem::path("renderSettings.mnsydata");


		int Msaa = GetMSAAIntValue();

		int thumbnailRes = GetThumbnailResolutionValue(m_thumbnailResolution);


		bool success;

		flcrm::JsonSettings renderSettings;
		renderSettings.FileOpen(success, renderSettingsFilePath, "Mnemosy Settings File", "Stores Render Settings");

		renderSettings.WriteInt(success, "renderSettings_MSAA", Msaa);
		renderSettings.WriteInt(success, "renderSettings_ThumbnailResolution", thumbnailRes);


		renderSettings.FilePrettyPrintSet(true);
		renderSettings.FileClose(success, renderSettingsFilePath);


	}

} // !mnemosy::graphics