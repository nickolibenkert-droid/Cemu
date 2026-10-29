#include "AndroidInputHelpers.h"
#include "JNIUtils.h"
#include "input/ControllerFactory.h"
#include "input/InputManager.h"
#include "input/api/Wiimote/WiimoteControllerProvider.h"
#include "input/api/Wiimote/android/AndroidUsbWiimote.h"

namespace NativeWiimote
{
	constexpr jint NO_WIIMOTE = -1;
	constexpr size_t MAX_WIIMOTES = 4;

	std::shared_ptr<WiimoteControllerProvider> GetProvider()
	{
		return std::dynamic_pointer_cast<WiimoteControllerProvider>(InputManager::instance().get_api_provider(InputAPI::Wiimote));
	}
} // namespace NativeWiimote

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeWiimote_setAdapter(JNIEnv* env, [[maybe_unused]] jclass clazz, jint fd, jintArray interfaceNumbers, jintArray inEndpoints)
{
	const jsize count = std::min(env->GetArrayLength(interfaceNumbers), env->GetArrayLength(inEndpoints));
	std::vector<jint> interfaces(count), endpoints(count);
	env->GetIntArrayRegion(interfaceNumbers, 0, count, interfaces.data());
	env->GetIntArrayRegion(inEndpoints, 0, count, endpoints.data());

	std::vector<AndroidUsbWiimote::SlotInfo> slots;
	slots.reserve(count);
	for (jsize i = 0; i < count; ++i)
	{
		slots.push_back({
			.interfaceNumber = static_cast<uint8>(interfaces[i]),
			.inEndpoint = static_cast<uint8>(endpoints[i]),
		});
	}
	AndroidUsbWiimote::SetAdapter(fd, slots);
}

extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeWiimote_clearAdapter([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	AndroidUsbWiimote::ClearAdapter();
}

extern "C" [[maybe_unused]] JNIEXPORT jbooleanArray JNICALL
Java_info_cemu_cemu_nativeinterface_NativeWiimote_getConnectedWiimotes(JNIEnv* env, [[maybe_unused]] jclass clazz)
{
	std::array<jboolean, NativeWiimote::MAX_WIIMOTES> connected{};
	if (auto provider = NativeWiimote::GetProvider())
	{
		for (size_t i = 0; i < connected.size(); ++i)
			connected[i] = provider->is_connected(i);
	}
	jbooleanArray result = env->NewBooleanArray(connected.size());
	env->SetBooleanArrayRegion(result, 0, connected.size(), connected.data());
	return result;
}

extern "C" [[maybe_unused]] JNIEXPORT jint JNICALL
Java_info_cemu_cemu_nativeinterface_NativeWiimote_getRealWiimote([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz, jint index)
{
	auto emulatedController = EmulatedControllerManager::GetController(index).GetControllerPtr();
	if (!emulatedController)
		return NativeWiimote::NO_WIIMOTE;

	for (const auto& controller : emulatedController->get_controllers())
	{
		if (controller->api() == InputAPI::Wiimote)
			return ConvertString<jint>(controller->uuid());
	}
	return NativeWiimote::NO_WIIMOTE;
}

// Puts a real Wii Remote behind emulated controller `index`, replacing its button mappings
// with the default one-to-one layout. wiimoteIndex -1 removes the real Wii Remote again
extern "C" [[maybe_unused]] JNIEXPORT void JNICALL
Java_info_cemu_cemu_nativeinterface_NativeWiimote_setRealWiimote([[maybe_unused]] JNIEnv* env, [[maybe_unused]] jclass clazz, jint index, jint wiimoteIndex)
{
	if (wiimoteIndex < NativeWiimote::NO_WIIMOTE || wiimoteIndex >= static_cast<jint>(NativeWiimote::MAX_WIIMOTES))
		return;

	auto emulatedController = EmulatedControllerManager::GetController(index).GetControllerPtr();
	if (!emulatedController)
		return;

	const auto controllers = emulatedController->get_controllers();
	for (const auto& controller : controllers)
	{
		if (controller->api() == InputAPI::Wiimote)
		{
			controller->stop_rumble();
			emulatedController->remove_controller(controller);
		}
	}

	if (wiimoteIndex < 0)
		return;

	auto wiimote = ControllerFactory::CreateController(InputAPI::Wiimote, fmt::format("{}", wiimoteIndex), "");
	wiimote->set_rumble(1.0f);
	emulatedController->clear_mappings();
	emulatedController->add_controller(wiimote);
	emulatedController->set_default_mapping(wiimote);
}
