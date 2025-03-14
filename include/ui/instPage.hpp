#pragma once
#include <pu/Plutonium>

using namespace pu::ui::elm;
namespace inst::ui {
	class instPage : public pu::ui::Layout
	{
	public:
		instPage();
		PU_SMART_CTOR(instPage)
			void onInput(u64 Down, u64 Up, u64 Held, pu::ui::TouchPoint touch_pos);
		TextBlock::Ref pageInfoText;
		TextBlock::Ref installInfoText;
		TextBlock::Ref sdInfoText;
		TextBlock::Ref nandInfoText;
		TextBlock::Ref countText;
		pu::ui::elm::ProgressBar::Ref installBar;
		static void setTopInstInfoText(std::string ourText);
		static void setInstInfoText(std::string ourText);
		static void filecount(std::string ourText);
		static void setInstBarPerc(double ourPercent);
		static void loadMainMenu();
		static void loadInstallScreen();
	private:
		Rectangle::Ref infoRect;
		Rectangle::Ref topRect;
		Image::Ref titleImage;
	};
}