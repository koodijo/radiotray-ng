// Copyright 2017 Edward G. Bruck <ed.bruck1@gmail.com>
//
// This file is part of Radiotray-NG.
//
// Radiotray-NG is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Radiotray-NG is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Radiotray-NG.  If not, see <http://www.gnu.org/licenses/>.

#include <radiotray-ng/common.hpp>
#include <radiotray-ng/extras/media_keys/media_keys.hpp>
#include <radiotray-ng/i_radiotray_ng.hpp>
#include <radiotray-ng/i_config.hpp>

#include <condition_variable>
#include <gio/gio.h>
#include <mutex>
#include <thread>
#include <map>


class media_keys_t
{
public:
	media_keys_t(std::shared_ptr<IRadioTrayNG> radiotray_ng, std::shared_ptr<IConfig> config)
		: radiotray_ng(std::move(radiotray_ng))
		, config(std::move(config))
		, app_name(std::string(APP_NAME) + "-" + std::to_string(::getpid()))
		, dbus_name("org.gnome.SettingsDaemon.MediaKeys")
	{
		// install extra media key mappings?
		if (this->config->get_bool(MEDIA_KEY_MAPPING_KEY, DEFAULT_MEDIA_KEY_MAPPING_VALUE))
		{
			this->media_keys[radiotray_ng::to_lower(this->config->get_string(MEDIA_KEY_VOLUME_UP_KEY, DEFAULT_MEDIA_KEY_VOLUME_UP_VALUE))] =
							std::bind(&IRadioTrayNG::volume_up_msg, this->radiotray_ng.get());

			this->media_keys[radiotray_ng::to_lower(this->config->get_string(MEDIA_KEY_VOLUME_DOWN_KEY, DEFAULT_MEDIA_KEY_VOLUME_DOWN_VALUE))] =
							std::bind(&IRadioTrayNG::volume_down_msg, this->radiotray_ng.get());

			this->media_keys[radiotray_ng::to_lower(this->config->get_string(MEDIA_KEY_NEXT_STAITON_KEY, DEFAULT_MEDIA_KEY_NEXT_STATION_VALUE))] =
				std::bind(&IRadioTrayNG::next_station_msg, this->radiotray_ng.get());

			this->media_keys[radiotray_ng::to_lower(this->config->get_string(MEDIA_KEY_PREVIOUS_STATION_KEY, DEFAULT_MEDIA_KEY_PREVIOUS_STATION_VALUE))] =
							std::bind(&IRadioTrayNG::previous_station_msg, this->radiotray_ng.get());

			this->log_media_keys();
		}

		// No entry is set, then check to see if GNOME is running...
		if (!this->config->exists(MEDIA_KEY_OLD_DBUS_NAME_KEY))
		{
			auto xdg_current_desktop = std::getenv("XDG_CURRENT_DESKTOP");

			if (xdg_current_desktop)
			{
				// if not gnome then assume unity or something else...
				if (radiotray_ng::to_lower(std::string(xdg_current_desktop)).find("gnome") == std::string::npos)
				{
					this->dbus_name = "org.gnome.SettingsDaemon";
				}
			}
			else
			{
				LOG(warning) << "could not read XDG_CURRENT_DESKTOP environment variable";
			}
		}
		else
		{
			if (this->config->get_bool(MEDIA_KEY_OLD_DBUS_NAME_KEY,	DEFAULT_MEDIA_KEY_OLD_DBUS_NAME_VALUE))
			{
				this->dbus_name = "org.gnome.SettingsDaemon";
			}
		}

		LOG(info) << "starting gio thread for: " << this->app_name << " using " << this->dbus_name;

		std::unique_lock<std::mutex> lock(this->gio_thread_mutex);

		this->gio_thread_func = std::thread(&media_keys_t::gio_thread, this);

		// wait for gio_thread to be ready...
		gio_thread_cv.wait(lock);
	}

	~media_keys_t()
	{
		LOG(info) << "stopping gio thread";

		this->gio_thread_cv.notify_one();

		if (this->gio_thread_func.joinable())
		{
			this->gio_thread_func.join();
		}
	}

	void log_media_keys()
	{
		LOG(info) << "mapping volume up/down to: "<< this->config->get_string(MEDIA_KEY_VOLUME_UP_KEY, DEFAULT_MEDIA_KEY_VOLUME_UP_VALUE) << ", "
			<< this->config->get_string(MEDIA_KEY_VOLUME_DOWN_KEY, DEFAULT_MEDIA_KEY_VOLUME_DOWN_VALUE);

		LOG(info) << "mapping station previous/next to: "<< this->config->get_string(MEDIA_KEY_PREVIOUS_STATION_KEY, DEFAULT_MEDIA_KEY_PREVIOUS_STATION_VALUE) << ", "
			<< this->config->get_string(MEDIA_KEY_NEXT_STAITON_KEY, DEFAULT_MEDIA_KEY_NEXT_STATION_VALUE);
	}

private:
	void gio_thread();

	static void on_gio_signal(GDBusProxy* proxy, gchar* sender_name, gchar* signal_name, GVariant* parameters, gpointer user_data);

	std::shared_ptr<IRadioTrayNG> radiotray_ng;
	std::shared_ptr<IConfig> config;
	const std::string app_name;
	std::string dbus_name;

	std::thread gio_thread_func;
	std::mutex  gio_thread_mutex;
	std::condition_variable gio_thread_cv;

	std::map<std::string, std::function<void ()>> media_keys;
};


void media_keys_t::on_gio_signal(GDBusProxy* /*proxy*/, gchar* /*sender_name*/, gchar* /*signal_name*/, GVariant* parameters, gpointer user_data)
{
	media_keys_t* media_keys{static_cast<media_keys_t*>(user_data)};

	if (parameters == nullptr || g_variant_n_children(parameters) != 2)
	{
		LOG(error) << "media key signal invalid, ignoring event";
		return;
	}

	// grab the key info
	std::string key_pressed;

	GVariant* tmp = g_variant_get_child_value(parameters, 1);
	const gchar* key = g_variant_get_string(tmp, NULL);

	if (key != nullptr)
	{
		key_pressed = key;

		LOG(debug) << "media key pressed: " << key_pressed;
	}
	else
	{
		LOG(error) << "failed to extract media key information, ignoring event";
		g_variant_unref(tmp);
		return;
	}
	g_variant_unref(tmp);

	// finally...
	if (key_pressed == "Stop")
	{
		media_keys->radiotray_ng->stop();
		return;
	}

	if (key_pressed == "Play")
	{
		if (media_keys->radiotray_ng->get_state() == STATE_STOPPED)
		{
			media_keys->radiotray_ng->play();
		}
		else
		{
			media_keys->radiotray_ng->stop();
		}
		return;
	}

	// use media key mapping?
	if (media_keys->config->get_bool(MEDIA_KEY_MAPPING_KEY, DEFAULT_MEDIA_KEY_MAPPING_VALUE))
	{
		auto it = media_keys->media_keys.find(radiotray_ng::to_lower(key_pressed));

		if (it != media_keys->media_keys.end())
		{
			it->second();
			return;
		}
	}

	LOG(debug) << "ignoring " << key_pressed;
}


void media_keys_t::gio_thread()
{
	GError*     error{nullptr};
	GDBusProxy* proxy{nullptr};

	proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION,
		GDBusProxyFlags{G_DBUS_PROXY_FLAGS_NONE},
		nullptr,
		this->dbus_name.c_str(),
		"/org/gnome/SettingsDaemon/MediaKeys",
		"org.gnome.SettingsDaemon.MediaKeys",
		nullptr,
		&error);

	if (proxy == nullptr)
	{
		LOG(error) << "could not connect to rtng_dbus, media keys disabled";

		this->gio_thread_cv.notify_one();
		return;
	}

	g_signal_connect(proxy, "g-signal", G_CALLBACK(on_gio_signal), this);

	g_dbus_proxy_call(proxy,
		"GrabMediaPlayerKeys",
		g_variant_new("(su)", this->app_name.c_str(), 0),
		G_DBUS_CALL_FLAGS_NO_AUTO_START,
		-1,
		nullptr,
		nullptr,
		nullptr);

	// signal that we are ready...
	this->gio_thread_cv.notify_one();

	std::unique_lock<std::mutex> lock(this->gio_thread_mutex);

	this->gio_thread_cv.wait(lock);

	// cleanup
	g_dbus_proxy_call(proxy,
		"ReleaseMediaPlayerKeys",
		g_variant_new("(s)", this->app_name.c_str()),
		G_DBUS_CALL_FLAGS_NO_AUTO_START,
		-1,
		nullptr,
		nullptr,
		nullptr);

	g_object_unref(proxy);
}


MediaKeys::MediaKeys(std::shared_ptr<IRadioTrayNG> radiotray_ng, std::shared_ptr<IConfig> config)
	: media_keys_impl(std::make_shared<media_keys_t>(radiotray_ng, config))
{
}