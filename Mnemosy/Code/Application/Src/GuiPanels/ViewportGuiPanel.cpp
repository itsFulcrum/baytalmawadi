#include "Include/GuiPanels/ViewportGuiPanel.h"

#include "Include/MnemosyEngine.h"
#include "Include/Core/Window.h"
#include "Include/Graphics/Renderer.h"


namespace mnemosy::gui
{
	ViewportGuiPanel::ViewportGuiPanel()
		: m_engineInstance{ MnemosyEngine::GetInstance() }
	{
		panelName = "Viewport";
		panelType = MNSY_GUI_PANEL_VIEWPORT;
	}

	void ViewportGuiPanel::Draw() {

		if (!showPanel)
			return;

		ImGui::Begin(panelName, &showPanel);
	
		DrawViewport();

		ImGui::End();
	}

	void ViewportGuiPanel::DrawViewport() {


		// width and height of the entrire imGui window including top bar and left padding
		m_windowSize = ImGui::GetWindowSize();

		// start position relative to the main glfw window
		m_windowPos = ImGui::GetWindowPos();

		// available size inside the ImGui window  use this to define the width and height of the texture image
		m_avail_size = ImGui::GetContentRegionAvail();

		m_imageSize = m_avail_size;

		// start position of viewport relative to the main glfw window
		m_viewportPosX = int(m_windowPos.x + (m_windowSize.x - m_avail_size.x));
		m_viewportPosY = int(m_windowPos.y + (m_windowSize.y - m_avail_size.y));


		unsigned int vSizeX = (unsigned int)m_imageSize.x;
		unsigned int vSizeY = (unsigned int)m_imageSize.y;
		
		// make sure we never have width/heigth of 0
		if (m_imageSize.x <= 0) {
			vSizeX = 1;
		}

		if (m_imageSize.y <= 0) {
			vSizeY = 1;
		}

		m_engineInstance.GetWindow().SetViewportData(vSizeX, vSizeY, m_viewportPosX, m_viewportPosY);

		// Display Rendered Frame

		// first casting to a uint64 is neccesary to get rid of waring for casting to void* in next line
		uint64_t textureID = (uint64_t)m_engineInstance.GetRenderer().GetRenderTextureId();
		ImGui::Image((void*)textureID, m_imageSize, ImVec2(0, 1), ImVec2(1, 0));


		// only pass input to Mnemosy if viewport window is hovered and docked
		if (ImGui::IsWindowHovered() && ImGui::IsWindowDocked()) {

			ImGui::CaptureMouseFromApp(false);
			ImGui::CaptureKeyboardFromApp(false);
		}


	}
}