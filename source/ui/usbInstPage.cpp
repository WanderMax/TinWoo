#include "ui/usbInstPage.hpp"
#include "ui/MainApplication.hpp"
#include "util/util.hpp"
#include "util/config.hpp"
#include "util/lang.hpp"
#include "usbInstall.hpp"


#define COLOR(hex) pu::ui::Color::FromHex(hex)

namespace inst::ui {
	extern MainApplication* mainApp;
	s32 www = 0; //touchscreen variable

	usbInstPage::usbInstPage() : Layout::Layout() {
		this->infoRect = Rectangle::New(0, 95, 1280, 60, COLOR("#00000080"));
		this->SetBackgroundColor(COLOR("#000000FF"));
		this->topRect = Rectangle::New(0, 0, 1280, 94, COLOR("#000000FF"));
		this->botRect = Rectangle::New(0, 659, 1280, 61, COLOR("#000000FF"));

		if (inst::config::useTheme && std::filesystem::exists(inst::config::appDir + "/images/Usb.png")) this->titleImage = Image::New(0, 0, (inst::config::appDir + "/images/Usb.png"));
		else this->titleImage = Image::New(0, 0, "romfs:/images/Usb.png");

		if (inst::config::useTheme && std::filesystem::exists(inst::config::appDir + "/images/Background.png")) this->SetBackgroundImage(inst::config::appDir + "/images/Background.png");
		else this->SetBackgroundImage("romfs:/images/Background.png");

		this->pageInfoText = TextBlock::New(10, 109, "");
		this->pageInfoText->SetColor(COLOR("#FFFFFFFF"));
		this->pageInfoText->SetFont(pu::ui::MakeDefaultFontName(30));
		this->butText = TextBlock::New(10, 678, "");
		this->butText->SetColor(COLOR("#FFFFFFFF"));
		this->menu = pu::ui::elm::Menu::New(0, 156, 1280, COLOR("#FFFFFF00"), COLOR("#4f4f4d33"), 84, (506 / 84));
		this->menu->SetItemsFocusColor(COLOR("#4f4f4dAA"));
		this->menu->SetScrollbarColor(COLOR("#1A1919FF"));
		this->infoImage = Image::New(460, 332, "romfs:/images/icons/usb-connection-waiting.png");
		this->Add(this->topRect);
		this->Add(this->infoRect);
		this->Add(this->botRect);
		this->Add(this->titleImage);
		this->Add(this->butText);
		this->Add(this->pageInfoText);
		this->Add(this->menu);
		this->Add(this->infoImage);
	}

	void usbInstPage::drawMenuItems_withext(bool clearItems) {
		int myindex = this->menu->GetSelectedIndex(); //store index so when page redraws we can get the last item we checked.
		if (clearItems) this->selectedTitles = {};
		this->menu->ClearItems();
		for (auto& url : this->ourTitles) {
			std::string itm = inst::util::shortenString(inst::util::formatUrlString(url), 56, true);
			auto ourEntry = pu::ui::elm::MenuItem::New(itm);
			ourEntry->SetColor(COLOR("#FFFFFFFF"));
			ourEntry->SetIcon("romfs:/images/icons/checkbox-blank-outline.png");
			for (long unsigned int i = 0; i < this->selectedTitles.size(); i++) {
				if (this->selectedTitles[i] == url) {
					ourEntry->SetIcon("romfs:/images/icons/check-box-outline.png");
				}
			}
			this->menu->AddItem(ourEntry);
			this->menu->SetSelectedIndex(myindex); //jump to the index we saved from above
		}
	}

	void usbInstPage::drawMenuItems(bool clearItems) {
		int myindex = this->menu->GetSelectedIndex(); //store index so when page redraws we can get the last item we checked.
		if (clearItems) this->selectedTitles = {};
		this->menu->ClearItems();
		for (auto& url : this->ourTitles) {

			std::string base_filename = url.substr(url.find_last_of("/") + 1); //just get the filename
			std::string::size_type const p(base_filename.find_last_of('.'));
			std::string file_without_extension = base_filename.substr(0, p); //strip of file extension
			std::string itm = inst::util::shortenString(inst::util::formatUrlString(file_without_extension), 56, true);

			//std::string itm = inst::util::shortenString(inst::util::formatUrlString(url), 56, true);
			auto ourEntry = pu::ui::elm::MenuItem::New(itm);
			ourEntry->SetColor(COLOR("#FFFFFFFF"));
			ourEntry->SetIcon("romfs:/images/icons/checkbox-blank-outline.png");
			for (long unsigned int i = 0; i < this->selectedTitles.size(); i++) {
				if (this->selectedTitles[i] == url) {
					ourEntry->SetIcon("romfs:/images/icons/check-box-outline.png");
				}
			}
			this->menu->AddItem(ourEntry);
			this->menu->SetSelectedIndex(myindex); //jump to the index we saved from above
		}
	}

	void usbInstPage::selectTitle(int selectedIndex) {
		if (this->menu->GetItems()[selectedIndex]->GetIconPath() == "romfs:/images/icons/check-box-outline.png") {
			for (long unsigned int i = 0; i < this->selectedTitles.size(); i++) {
				if (this->selectedTitles[i] == this->ourTitles[selectedIndex]) this->selectedTitles.erase(this->selectedTitles.begin() + i);
			}
		}
		else this->selectedTitles.push_back(this->ourTitles[selectedIndex]);
		this->drawMenuItems(false);
	}

	void usbInstPage::startUsb() {
		this->pageInfoText->SetText("inst.usb.top_info"_lang);
		this->butText->SetText("inst.usb.buttons"_lang);
		this->menu->SetVisible(false);
		this->menu->ClearItems();
		this->infoImage->SetVisible(true);
		mainApp->LoadLayout(mainApp->usbinstPage);
		mainApp->CallForRender();
		this->ourTitles = usbInstStuff::OnSelected();
		if (!this->ourTitles.size()) {
			mainApp->LoadLayout(mainApp->mainPage);
			return;
		}
		else {
			mainApp->CallForRender(); // If we re-render a few times during this process the main screen won't flicker
			this->pageInfoText->SetText("inst.usb.top_info2"_lang);
			this->butText->SetText("inst.usb.buttons2"_lang);
			this->drawMenuItems(true);
			this->menu->SetSelectedIndex(0);
			mainApp->CallForRender();
			this->infoImage->SetVisible(false);
			this->menu->SetVisible(true);
		}
		return;
	}

	void usbInstPage::startInstall() {
		int dialogResult = -1;
		if (this->selectedTitles.size() == 1) {
			dialogResult = mainApp->CreateShowDialog("inst.target.desc0"_lang + ":\n\n" + inst::util::shortenString(std::filesystem::path(this->selectedTitles[0]).filename().string(), 32, true) + "\n\n" + "inst.target.desc1"_lang, "\n\n\n\n\n\n\n" + "common.cancel_desc"_lang, { "inst.target.opt0"_lang, "inst.target.opt1"_lang }, false, "romfs:/images/icons/install.png");
		}
		//else dialogResult = mainApp->CreateShowDialog("inst.target.desc00"_lang + std::to_string(this->selectedTitles.size()) + "inst.target.desc01"_lang, "common.cancel_desc"_lang, { "inst.target.opt0"_lang, "inst.target.opt1"_lang }, false);
		else dialogResult = mainApp->CreateShowDialog("inst.target.desc00"_lang + std::to_string(this->selectedTitles.size()) + "inst.target.desc01"_lang, "\n" + "common.cancel_desc"_lang, { "inst.target.opt0"_lang, "inst.target.opt1"_lang }, false, "romfs:/images/icons/install.png");
		if (dialogResult == -1) return;
		usbInstStuff::installTitleUsb(this->selectedTitles, dialogResult);
		return;
	}

	void usbInstPage::onInput(u64 Down, u64 Up, u64 Held, pu::ui::TouchPoint touch_pos) {
		if (Down & HidNpadButton_B) {
			mainApp->LoadLayout(mainApp->mainPage);
		}

		HidTouchScreenState state = { 0 };

		if (hidGetTouchScreenStates(&state, 1)) {

			if ((Down & HidNpadButton_A) || (state.count != www))
			{
				www = state.count;

				if (www != 1) {
					int var = this->menu->GetItems().size();
					auto s = std::to_string(var);
					if (s == "0") {
						//do nothing here because there's no items in the list, that way the app won't freeze
					}
					else {
						this->selectTitle(this->menu->GetSelectedIndex());
						if (this->menu->GetItems().size() == 1 && this->selectedTitles.size() == 1) {
							this->startInstall();
						}
					}
				}
			}
		}

		if ((Down & HidNpadButton_Y)) {
			if (this->selectedTitles.size() == this->menu->GetItems().size()) this->drawMenuItems(true);
			else {
				for (long unsigned int i = 0; i < this->menu->GetItems().size(); i++) {
					if (this->menu->GetItems()[i]->GetIconPath() == "romfs:/images/icons/check-box-outline.png") continue;
					else this->selectTitle(i);
				}
				this->drawMenuItems(false);
			}
		}

		if (Down & HidNpadButton_Plus) {
			int var = this->menu->GetItems().size();
			auto s = std::to_string(var);

			if (s == "0") {
				//do nothing here because there's no items in the list, that way the app won't freeze
			}

			else {
				if (this->selectedTitles.size() == 0) {
					this->selectTitle(this->menu->GetSelectedIndex());
					this->startInstall();
					return;
				}
				this->startInstall();
			}
		}

		if (Down & HidNpadButton_ZL)
			this->menu->SetSelectedIndex(std::max(0, this->menu->GetSelectedIndex() - 6));

		if (Down & HidNpadButton_ZR)
			this->menu->SetSelectedIndex(std::min((s32)this->menu->GetItems().size() - 1, this->menu->GetSelectedIndex() + 6));

		//goto top of list	
		if (Down & HidNpadButton_L) {
			int x = this->menu->GetItems().size() - 1;
			this->menu->SetSelectedIndex(std::max(0, this->menu->GetSelectedIndex() - x));
		}

		//goto bottom of list
		if (Down & HidNpadButton_R) {
			int x = this->menu->GetItems().size() - 1;
			this->menu->SetSelectedIndex(std::min((s32)this->menu->GetItems().size() - 1, this->menu->GetSelectedIndex() + x));
		}

		//don't show file extensions
		if (Down & HidNpadButton_Left) {
			this->drawMenuItems(true);
		}

		//show file extensions
		if (Down & HidNpadButton_Right) {
			this->drawMenuItems_withext(true);
		}
	}
}