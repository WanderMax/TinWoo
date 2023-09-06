#include <filesystem>
#include <iostream>
#include <sstream>
#include <switch.h>
#include "ui/MainApplication.hpp"
#include "ui/mainPage.hpp"
#include "util/util.hpp"
#include "util/config.hpp"
#include "util/lang.hpp"
#include "sigInstall.hpp"
#include "HDInstall.hpp"
#include "data/buffered_placeholder_writer.hpp"
#include "nx/usbhdd.h"
#include "usbhsfs.h"
#include <sys/statvfs.h>

#define COLOR(hex) pu::ui::Color::FromHex(hex)

int statvfs(const char* path, struct statvfs* buf);
s32 prev_touchcount = 0;

double GetAvailableSpace(const char* path)
{
	struct statvfs stat;

	if (statvfs(path, &stat) != 0) {
		// error happens, just quits here
		return -1;
	}

	// the available size is f_bsize * f_bavail
	return stat.f_bsize * stat.f_bavail;
}

double amountOfDiskSpaceUsed(const char* path)
{
	struct statvfs stat;

	if (statvfs(path, &stat) != 0) {
		// error happens, just quits here
		return -1;
	}
	const auto total = static_cast<unsigned long>(stat.f_blocks);
	const auto available = static_cast<unsigned long>(stat.f_bavail);
	const auto availableToRoot = static_cast<unsigned long>(stat.f_bfree);
	const auto used = total - availableToRoot;
	const auto nonRootTotal = used + available;
	return 100.0 * static_cast<double>(used) / static_cast<double>(nonRootTotal);
}

double totalsize(const char* path)
{
	struct statvfs stat;

	if (statvfs(path, &stat) != 0) {
		// error happens, just quits here
		return -1;
	}
	return stat.f_blocks * stat.f_frsize;
}

namespace inst::ui {
	extern MainApplication* mainApp;
	bool appletFinished = false;
	bool updateFinished = false;

	void mathstuff() {
		double math = (GetAvailableSpace("./") / 1024) / 1024; //megabytes
		float math2 = ((float)math / 1024); //gigabytes

		double used = (amountOfDiskSpaceUsed("./")); //same file path as sdmc

		double total = (totalsize("sdmc:/") / 1024) / 1024; //megabytes
		float total2 = ((float)total / 1024); //gigabytes
		//
		float GB = math2;
		std::stringstream stream;
		stream << std::fixed << std::setprecision(2) << GB; //only show 2 decimal places
		std::string freespace = stream.str();


		float GB2 = total2;
		std::stringstream stream2;
		stream2 << std::fixed << std::setprecision(2) << GB2; //only show 2 decimal places
		std::string sdsize = stream2.str();

		//printf("\nSdCard Free Space in MB: %li", math);
		//printf("\nSdCard Free Space in GB: %.2f", math2);
		std::stringstream stream3;
		stream3 << std::fixed << std::setprecision(2) << used; //only show 2 decimal places
		std::string percent = stream3.str();

		//unmount sd here and mount system....
		//fsdevUnmountDevice("sdmc");
		FsFileSystem nandFS;
		fsOpenBisFileSystem(&nandFS, FsBisPartitionId_User, "");
		fsdevMountDevice("user", nandFS);

		double math3 = (GetAvailableSpace("user:/") / 1024) / 1024; //megabytes
		float math4 = ((float)math3 / 1024); //gigabytes

		double used2 = (amountOfDiskSpaceUsed("user:/")); //same file path as sdmc

		double total3 = (totalsize("user:/") / 1024) / 1024; //megabytes
		float total4 = ((float)total3 / 1024); //gigabytes
		//
		float GB3 = math4;
		std::stringstream stream4;
		stream4 << std::fixed << std::setprecision(2) << GB3; //only show 2 decimal places
		std::string freespace2 = stream4.str();


		float GB4 = total4;
		std::stringstream stream5;
		stream5 << std::fixed << std::setprecision(2) << GB4; //only show 2 decimal places
		std::string sdsize2 = stream5.str();

		//printf("\nSdCard Free Space in MB: %li", math);
		//printf("\nSdCard Free Space in GB: %.2f", math2);
		std::stringstream stream6;
		stream6 << std::fixed << std::setprecision(2) << used2; //only show 2 decimal places
		std::string percent2 = stream6.str();

		//unmount user now as we already know how much space we have	
		fsdevUnmountDevice("user");

		std::string Info = ("System total size: " + sdsize2 + " GB" + "\nSystem free space: " + freespace2 + " GB" + "\nSystem percent used: " + percent2 + "%" + "\n\n" + "SD card total size: " + sdsize + " GB" + "\nSD card free space: " + freespace + " GB" + "\nSD card percent used: " + percent + "%");
		//std::string Info = ("System total size: " + sdsize2 + " GB" + "\nSystem free space: " + freespace2 + " GB" + "\nSystem percent used: " + percent2 + "%" + "\n\n" + "SD card total size: " + sdsize + " GB" + "\nSD card free space: " + freespace + " GB" + "\nSD card percent used: " + percent + "%");
		inst::ui::mainApp->CreateShowDialog("Space Usage Information", Info, { "common.ok"_lang }, true, "romfs:/images/icons/drive.png");
	}

	void mainMenuThread() {
		bool menuLoaded = mainApp->IsShown();
		if (!appletFinished && appletGetAppletType() == AppletType_LibraryApplet) {
			tin::data::NUM_BUFFER_SEGMENTS = 2;
			if (menuLoaded) {
				inst::ui::appletFinished = true;
				mainApp->CreateShowDialog("main.applet.title"_lang, "main.applet.desc"_lang, { "common.ok"_lang }, true, "romfs:/images/icons/information.png");
			}
		}
		else if (!appletFinished) {
			inst::ui::appletFinished = true;
			tin::data::NUM_BUFFER_SEGMENTS = 128;
		}
		if (!updateFinished && (!inst::config::autoUpdate || inst::util::getIPAddress() == "1.0.0.127")) updateFinished = true;

		if (!updateFinished && menuLoaded && inst::config::updateInfo.size()) {
			updateFinished = true;
			optionsPage::askToUpdate(inst::config::updateInfo);
		}
	}

	MainPage::MainPage() : Layout::Layout() {
		this->SetBackgroundColor(COLOR("#000000FF"));
		this->topRect = Rectangle::New(0, 0, 1280, 94, COLOR("#000000FF"));
		this->botRect = Rectangle::New(0, 659, 1280, 61, COLOR("#000000FF"));

		if (inst::config::gayMode) {
			if (std::filesystem::exists(inst::config::appDir + "/images/Main.png")) this->titleImage = Image::New(0, 0, (inst::config::appDir + "/images/Main.png"));
			else
				this->titleImage = Image::New(0, 0, "romfs:/images/Main.png");
			if (std::filesystem::exists(inst::config::appDir + "/images/Background.png")) this->SetBackgroundImage(inst::config::appDir + "/images/Background.png");
			else
				this->SetBackgroundImage("romfs:/images/Background.png");
		}
		else {
			this->SetBackgroundImage("romfs:/images/Background.png");
			this->titleImage = Image::New(0, 0, "romfs:/images/Main.png");
		}
		this->butText = TextBlock::New(10, 678, "main.buttons"_lang);
		this->butText->SetColor(COLOR("#FFFFFFFF"));
		this->optionMenu = pu::ui::elm::Menu::New(0, 95, 1280, COLOR("#FFFFFF00"), COLOR("#4f4f4d33"), 94, 6);
		this->optionMenu->SetItemsFocusColor(COLOR("#4f4f4dAA"));
		this->optionMenu->SetScrollbarColor(COLOR("#1A1919FF"));
		this->installMenuItem = pu::ui::elm::MenuItem::New("main.menu.sd"_lang);
		this->installMenuItem->SetColor(COLOR("#FFFFFFFF"));
		this->installMenuItem->SetIcon("romfs:/images/icons/micro-sd.png");
		this->netInstallMenuItem = pu::ui::elm::MenuItem::New("main.menu.net"_lang);
		this->netInstallMenuItem->SetColor(COLOR("#FFFFFFFF"));
		this->netInstallMenuItem->SetIcon("romfs:/images/icons/cloud-download.png");
		this->usbInstallMenuItem = pu::ui::elm::MenuItem::New("main.menu.usb"_lang);
		this->usbInstallMenuItem->SetColor(COLOR("#FFFFFFFF"));
		this->usbInstallMenuItem->SetIcon("romfs:/images/icons/usb-port.png");
		this->HdInstallMenuItem = pu::ui::elm::MenuItem::New("main.menu.hdd"_lang);
		this->HdInstallMenuItem->SetColor(COLOR("#FFFFFFFF"));
		this->HdInstallMenuItem->SetIcon("romfs:/images/icons/usb-hd.png");
		this->settingsMenuItem = pu::ui::elm::MenuItem::New("main.menu.set"_lang);
		this->settingsMenuItem->SetColor(COLOR("#FFFFFFFF"));
		this->settingsMenuItem->SetIcon("romfs:/images/icons/settings.png");
		this->exitMenuItem = pu::ui::elm::MenuItem::New("main.menu.exit"_lang);
		this->exitMenuItem->SetColor(COLOR("#FFFFFFFF"));
		this->exitMenuItem->SetIcon("romfs:/images/icons/exit-run.png");
		if (inst::config::gayMode) {
			if (std::filesystem::exists(inst::config::appDir + "/images/Main.png")) this->awooImage = Image::New(0, 0, inst::config::appDir + "/images/Main.png");
			else this->awooImage = Image::New(0, 0, "romfs:/images/Main.png");
		}
		else {
			this->awooImage = Image::New(0, 0, "romfs:/images/Main.png");
		}
		this->eggImage = Image::New(0, 0, "");
		this->Add(this->topRect);
		this->Add(this->botRect);
		this->Add(this->titleImage);
		this->Add(this->butText);
		this->optionMenu->AddItem(this->installMenuItem);
		this->optionMenu->AddItem(this->netInstallMenuItem);
		this->optionMenu->AddItem(this->usbInstallMenuItem);
		this->optionMenu->AddItem(this->HdInstallMenuItem);
		this->optionMenu->AddItem(this->settingsMenuItem);
		this->optionMenu->AddItem(this->exitMenuItem);
		if (nx::hdd::count() && nx::hdd::rootPath()) {
			this->hdd = Image::New(1156, 669, "romfs:/images/icons/usb-hd-connected.png");
			this->Add(this->hdd);
		}
		this->Add(this->awooImage);
		this->Add(this->eggImage);
		this->awooImage->SetVisible(!inst::config::gayMode);
		this->Add(this->optionMenu);
		this->AddRenderCallback(mainMenuThread);
	}

	void MainPage::installMenuItem_Click() {
		mainApp->sdinstPage->drawMenuItems(true, "sdmc:/");
		mainApp->sdinstPage->menu->SetSelectedIndex(0);
		mainApp->LoadLayout(mainApp->sdinstPage);
	}

	void MainPage::netInstallMenuItem_Click() {
		if (inst::util::getIPAddress() == "1.0.0.127") {
			inst::ui::mainApp->CreateShowDialog("main.net.title"_lang, "main.net.desc"_lang, { "common.ok"_lang }, true, "romfs:/images/icons/information.png");
			return;
		}
		mainApp->netinstPage->startNetwork();
	}

	void MainPage::usbInstallMenuItem_Click() {
		if (!inst::config::usbAck) {
			if (mainApp->CreateShowDialog("main.usb.warn.title"_lang, "main.usb.warn.desc"_lang, { "common.ok"_lang, "main.usb.warn.opt1"_lang }, false, "romfs:/images/icons/usb.png") == 1) {
				inst::config::usbAck = true;
				inst::config::setConfig();
			}
		}
		if (inst::util::getUsbState() == 5) mainApp->usbinstPage->startUsb();
		else mainApp->CreateShowDialog("main.usb.error.title"_lang, "main.usb.error.desc"_lang, { "common.ok"_lang }, false, "romfs:/images/icons/usb.png");
	}

	void MainPage::HdInstallMenuItem_Click() {
		if (nx::hdd::count() && nx::hdd::rootPath()) {
			mainApp->HDinstPage->drawMenuItems(true, nx::hdd::rootPath());
			mainApp->HDinstPage->menu->SetSelectedIndex(0);
			mainApp->LoadLayout(mainApp->HDinstPage);
		}
		else {
			inst::ui::mainApp->CreateShowDialog("main.hdd.title"_lang, "main.hdd.notfound"_lang, { "common.ok"_lang }, true, "romfs:/images/icons/drive.png");
		}
	}

	void MainPage::exitMenuItem_Click() {
		mainApp->FadeOut();
		mainApp->Close();
	}

	void MainPage::settingsMenuItem_Click() {
		mainApp->LoadLayout(mainApp->optionspage);
	}

	void MainPage::onInput(u64 Down, u64 Up, u64 Held, pu::ui::TouchPoint touch_pos) {

		if (((Down & HidNpadButton_Plus) || (Down & HidNpadButton_Minus) || ((Held & HidNpadButton_L) && (Down & HidNpadButton_R)) || ((Down & HidNpadButton_L) && (Held & HidNpadButton_R))) && mainApp->IsShown()) {
			mainApp->FadeOut();
			mainApp->Close();
		}

		HidTouchScreenState state = { 0 };

		if (hidGetTouchScreenStates(&state, 1)) {

			if ((Down & HidNpadButton_A) || (state.count != prev_touchcount))
			{
				prev_touchcount = state.count;

				if (prev_touchcount != 1) {
					int menuindex = this->optionMenu->GetSelectedIndex();
					switch (menuindex) {
					case 0:
						this->installMenuItem_Click();
						break;
					case 1:
						this->netInstallMenuItem_Click();
						break;
					case 2:
						MainPage::usbInstallMenuItem_Click();
						break;
					case 3:
						MainPage::HdInstallMenuItem_Click();
						break;
					case 4:
						MainPage::settingsMenuItem_Click();
						break;
					case 5:
						MainPage::exitMenuItem_Click();
						break;
					default:
						break;
					}
				}
			}
		}

		if (Down & HidNpadButton_X) {
			this->awooImage->SetVisible(false);
			this->eggImage->SetVisible(true);
		}

		if (Up & HidNpadButton_A) {
			this->eggImage->SetVisible(false);
			if (!inst::config::gayMode) this->awooImage->SetVisible(true);
		}

		if (Down & HidNpadButton_Y) {
			mathstuff();
		}

		if (Down & HidNpadButton_ZL) {
		}

		if (Down & HidNpadButton_ZR) {
		}

		if (Down & HidNpadButton_L) {
		}

		if (Down & HidNpadButton_R) {
		}
	}
}