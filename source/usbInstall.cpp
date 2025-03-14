/*
Copyright (c) 2017-2018 Adubbz

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <string>
#include <thread>
#include <malloc.h>
#include "usbInstall.hpp"
#include "install/usb_nsp.hpp"
#include "install/install_nsp.hpp"
#include "install/usb_xci.hpp"
#include "install/install_xci.hpp"
#include "util/error.hpp"
#include "util/usb_util.hpp"
#include "util/util.hpp"
#include "util/config.hpp"
#include "util/lang.hpp"
#include "ui/MainApplication.hpp"
#include "ui/usbInstPage.hpp"
#include "ui/instPage.hpp"
#include "util/theme.hpp"

namespace inst::ui {
	extern MainApplication* mainApp;
}

namespace inst::ui {
	std::string usbi_root = inst::config::appDir + "/theme";
	bool usbi_theme = util::themeit(usbi_root); //check if we have a previous theme directory first.
}

namespace usbInstStuff {
	struct TUSHeader
	{
		u32 magic; // TUL0 (Tinfoil Usb List 0)
		u32 titleListSize;
		u64 padding;
	} PACKED;

	int bufferData(void* buf, size_t size, u64 timeout = 5000000000)
	{
		u8* tempBuffer = (u8*)memalign(0x1000, size);
		if (tin::util::USBRead(tempBuffer, size, timeout) == 0) return 0;
		memcpy(buf, tempBuffer, size);
		free(tempBuffer);
		return size;
	}

	std::vector<std::string> OnSelected() {
		TUSHeader header;

		padConfigureInput(8, HidNpadStyleSet_NpadStandard);
		PadState pad;
		padInitializeAny(&pad);
		std::string info = "romfs:/images/icons/information.png";
		if (inst::ui::usbi_theme && inst::config::useTheme && std::filesystem::exists(inst::config::appDir + "/theme/theme.json") && std::filesystem::exists(inst::config::appDir + "icons_others.information"_theme)) {
			info = inst::config::appDir + "icons_others.information"_theme;
		}

		while (true) {
			if (bufferData(&header, sizeof(TUSHeader), 500000000) != 0) break;

			padUpdate(&pad);
			u64 kDown = padGetButtonsDown(&pad);

			if (kDown & HidNpadButton_B) return {};
			if (kDown & HidNpadButton_X) inst::ui::mainApp->CreateShowDialog("inst.usb.help.title"_lang, "inst.usb.help.desc"_lang, { "common.ok"_lang }, true, info);
			if (inst::util::getUsbState() != 5) return {};
		}

		if (header.magic != 0x304C5554) return {};

		std::vector<std::string> titleNames;
		char* titleNameBuffer = (char*)memalign(0x1000, header.titleListSize + 1);
		memset(titleNameBuffer, 0, header.titleListSize + 1);

		tin::util::USBRead(titleNameBuffer, header.titleListSize, 10000000000);

		// Split the string up into individual title names
		std::stringstream titleNamesStream(titleNameBuffer);
		std::string segment;
		while (std::getline(titleNamesStream, segment, '\n')) titleNames.push_back(segment);
		free(titleNameBuffer);
		std::sort(titleNames.begin(), titleNames.end(), inst::util::ignoreCaseCompare);

		return titleNames;
	}

	void installTitleUsb(std::vector<std::string> ourTitleList, int ourStorage)
	{
		inst::util::initInstallServices();
		inst::ui::instPage::loadInstallScreen();
		bool nspInstalled = true;
		NcmStorageId m_destStorageId = NcmStorageId_SdCard;

		std::string good = "romfs:/images/icons/good.png";
		if (inst::ui::usbi_theme && inst::config::useTheme && std::filesystem::exists(inst::config::appDir + "/theme/theme.json") && std::filesystem::exists(inst::config::appDir + "icons_others.good"_theme)) {
			good = inst::config::appDir + "icons_others.good"_theme;
		}
		std::string fail = "romfs:/images/icons/fail.png";
		if (inst::ui::usbi_theme && inst::config::useTheme && std::filesystem::exists(inst::config::appDir + "/theme/theme.json") && std::filesystem::exists(inst::config::appDir + "icons_others.fail"_theme)) {
			fail = inst::config::appDir + "icons_others.fail"_theme;
		}

		if (ourStorage) m_destStorageId = NcmStorageId_BuiltInUser;
		unsigned int fileItr;

		std::vector<std::string> fileNames;
		for (long unsigned int i = 0; i < ourTitleList.size(); i++) {
			fileNames.push_back(inst::util::shortenString(inst::util::formatUrlString(ourTitleList[i]), 40, true));
		}

		std::vector<int> previousClockValues;
		if (inst::config::overClock) {
			previousClockValues.push_back(inst::util::setClockSpeed(0, 1785000000)[0]);
			previousClockValues.push_back(inst::util::setClockSpeed(1, 76800000)[0]);
			previousClockValues.push_back(inst::util::setClockSpeed(2, 1600000000)[0]);
		}

		try {
			int togo = ourTitleList.size();
			for (fileItr = 0; fileItr < ourTitleList.size(); fileItr++) {
				auto s = std::to_string(togo);
				inst::ui::instPage::filecount("inst.info_page.queue"_lang + s);
				inst::ui::instPage::setTopInstInfoText("inst.info_page.top_info0"_lang + fileNames[fileItr] + "inst.usb.source_string"_lang);
				std::unique_ptr<tin::install::Install> installTask;

				if (ourTitleList[fileItr].compare(ourTitleList[fileItr].size() - 3, 2, "xc") == 0) {
					auto usbXCI = std::make_shared<tin::install::xci::USBXCI>(ourTitleList[fileItr]);
					installTask = std::make_unique<tin::install::xci::XCIInstallTask>(m_destStorageId, inst::config::ignoreReqVers, usbXCI);
				}
				else {
					auto usbNSP = std::make_shared<tin::install::nsp::USBNSP>(ourTitleList[fileItr]);
					installTask = std::make_unique<tin::install::nsp::NSPInstall>(m_destStorageId, inst::config::ignoreReqVers, usbNSP);
				}

				LOG_DEBUG("%s\n", "Preparing installation");
				inst::ui::instPage::setInstInfoText("inst.info_page.preparing"_lang);
				inst::ui::instPage::setInstBarPerc(0);
				installTask->Prepare();
				installTask->InstallTicketCert();
				installTask->Begin();
				togo = (togo - 1);
			}

			inst::ui::instPage::filecount("inst.info_page.queue"_lang + "0");
		}
		catch (std::exception& e) {
			LOG_DEBUG("Failed to install");
			LOG_DEBUG("%s", e.what());
			fprintf(stdout, "%s", e.what());
			inst::ui::instPage::setInstInfoText("inst.info_page.failed"_lang + fileNames[fileItr]);
			inst::ui::instPage::setInstBarPerc(0);

			if (inst::config::useSound) {
				std::string audioPath = "romfs:/audio/fail.mp3";
				std::string fail = inst::config::appDir + "audio.fail"_theme;
				if (inst::ui::usbi_theme && inst::config::useTheme && std::filesystem::exists(inst::config::appDir + "/theme/theme.json") && std::filesystem::exists(fail)) audioPath = (fail);
				std::thread audioThread(inst::util::playAudio, audioPath);
				audioThread.join();
			}

			inst::ui::mainApp->CreateShowDialog("inst.info_page.failed"_lang + fileNames[fileItr] + "!", "inst.info_page.failed_desc"_lang + "\n\n" + (std::string)e.what(), { "common.ok"_lang }, true, fail);
			nspInstalled = false;
		}

		if (previousClockValues.size() > 0) {
			inst::util::setClockSpeed(0, previousClockValues[0]);
			inst::util::setClockSpeed(1, previousClockValues[1]);
			inst::util::setClockSpeed(2, previousClockValues[2]);
		}

		if (nspInstalled) {
			tin::util::USBCmdManager::SendExitCmd();
			inst::ui::instPage::setInstInfoText("inst.info_page.complete"_lang);
			inst::ui::instPage::setInstBarPerc(100);

			if (inst::config::useSound) {
				std::string audioPath = "romfs:/audio/pass.mp3";
				std::string pass = inst::config::appDir + "audio.pass"_theme;
				if (inst::ui::usbi_theme && inst::config::useTheme && std::filesystem::exists(inst::config::appDir + "/theme/theme.json") && std::filesystem::exists(pass)) audioPath = (pass);
				std::thread audioThread(inst::util::playAudio, audioPath);
				audioThread.join();
			}

			if (ourTitleList.size() > 1) inst::ui::mainApp->CreateShowDialog(std::to_string(ourTitleList.size()) + "inst.info_page.desc0"_lang, Language::GetRandomMsg(), { "common.ok"_lang }, true, good);
			else inst::ui::mainApp->CreateShowDialog(fileNames[0] + "inst.info_page.desc1"_lang, Language::GetRandomMsg(), { "common.ok"_lang }, true, good);
		}

		LOG_DEBUG("Done");
		inst::ui::instPage::loadMainMenu();
		inst::util::deinitInstallServices();
		return;
	}
}