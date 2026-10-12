// MIMIC — éditeur Windows : une fenêtre enfant HWND qui héberge Microsoft Edge WebView2 et affiche l'interface (ui/mimic_ui.html,
// embarquée dans la DLL comme ressource RCDATA 101). Aucun accès réseau requis pour la page elle-même.
#include "editor.h"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <wrl/client.h>
#include <wrl/event.h>

#include "WebView2.h"
#include "editor_bridge.h"
#include "public.sdk/source/common/pluginview.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>

extern void* moduleHandle; // défini par le point d'entrée DLL du SDK VST3

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace mimic {
namespace {

constexpr UINT_PTR kTimerId = 1;
constexpr int kResourceId = 101;

std::wstring widen (const std::string& s)
{
	if (s.empty ())
		return {};
	const int n = MultiByteToWideChar (CP_UTF8, 0, s.data (), static_cast<int> (s.size ()), nullptr, 0);
	std::wstring w (static_cast<size_t> (n), L'\0');
	MultiByteToWideChar (CP_UTF8, 0, s.data (), static_cast<int> (s.size ()), w.data (), n);
	return w;
}

std::string narrow (const wchar_t* w)
{
	if (!w || !*w)
		return {};
	const int n = WideCharToMultiByte (CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	std::string s (static_cast<size_t> (n > 0 ? n - 1 : 0), '\0');
	WideCharToMultiByte (CP_UTF8, 0, w, -1, s.data (), n, nullptr, nullptr);
	return s;
}

std::string loadUiHtml ()
{
	const auto hm = static_cast<HMODULE> (moduleHandle);
	HRSRC r = FindResourceW (hm, MAKEINTRESOURCEW (kResourceId), RT_RCDATA);
	if (!r)
		return {};
	HGLOBAL g = LoadResource (hm, r);
	if (!g)
		return {};
	const char* p = static_cast<const char*> (LockResource (g));
	return std::string (p, p + SizeofResource (hm, r));
}

// Tout l'état fenêtre/WebView2. Les rappels asynchrones ne tiennent qu'un weak_ptr : pas d'accès après destruction.
struct Impl : std::enable_shared_from_this<Impl>
{
	Controller* controller = nullptr;
	HWND hwnd = nullptr;
	std::wstring className;
	bool comInit = false;
	ComPtr<ICoreWebView2Controller> wvController;
	ComPtr<ICoreWebView2> webview;
	std::unique_ptr<EditorBridge> bridge;
	std::wstring status = L"MIMIC — chargement…";
	EventRegistrationToken msgToken {};

	void layout ()
	{
		if (wvController && hwnd)
		{
			RECT rc;
			GetClientRect (hwnd, &rc);
			wvController->put_Bounds (rc);
		}
	}

	void startWebView ()
	{
		std::weak_ptr<Impl> weak = shared_from_this ();
		wchar_t base[MAX_PATH] = {};
		GetEnvironmentVariableW (L"LOCALAPPDATA", base, MAX_PATH);
		std::wstring dataDir = std::wstring (base) + L"\\MIMIC\\WebView2";

		HRESULT hr = CreateCoreWebView2EnvironmentWithOptions (
		    nullptr, dataDir.c_str (), nullptr,
		    Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> ([weak] (HRESULT res, ICoreWebView2Environment* env) -> HRESULT {
			    auto self = weak.lock ();
			    if (!self)
				    return S_OK;
			    if (FAILED (res) || !env)
			    {
				    self->fail (L"Impossible de démarrer WebView2.");
				    return S_OK;
			    }
			    env->CreateCoreWebView2Controller (
			        self->hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> ([weak] (HRESULT res2, ICoreWebView2Controller* ctl) -> HRESULT {
				        auto s2 = weak.lock ();
				        if (!s2)
					        return S_OK;
				        if (FAILED (res2) || !ctl)
				        {
					        s2->fail (L"Impossible de créer la vue WebView2.");
					        return S_OK;
				        }
				        s2->onControllerReady (ctl);
				        return S_OK;
			        }).Get ());
			    return S_OK;
		    }).Get ());
		if (FAILED (hr))
			fail (L"Microsoft Edge WebView2 Runtime est introuvable.\nInstalle-le (gratuit, fourni avec Windows 11) puis rouvre MIMIC.");
	}

	void fail (const wchar_t* msg)
	{
		status = std::wstring (L"MIMIC\n\n") + msg;
		if (hwnd)
			InvalidateRect (hwnd, nullptr, TRUE);
	}

	void onControllerReady (ICoreWebView2Controller* ctl)
	{
		wvController = ctl;
		wvController->get_CoreWebView2 (&webview);
		if (!webview)
		{
			fail (L"WebView2 indisponible.");
			return;
		}
		ComPtr<ICoreWebView2Controller2> c2;
		if (SUCCEEDED (wvController.As (&c2)))
		{
			COREWEBVIEW2_COLOR bg = {255, 7, 6, 7};
			c2->put_DefaultBackgroundColor (bg);
		}
		ComPtr<ICoreWebView2Settings> st;
		if (SUCCEEDED (webview->get_Settings (&st)))
		{
			st->put_AreDefaultContextMenusEnabled (FALSE);
			st->put_AreDevToolsEnabled (FALSE);
			st->put_IsZoomControlEnabled (FALSE);
			st->put_IsStatusBarEnabled (FALSE);
			ComPtr<ICoreWebView2Settings3> st3;
			if (SUCCEEDED (st.As (&st3)))
				st3->put_AreBrowserAcceleratorKeysEnabled (FALSE);
		}

		// pont : messages de la page
		bridge = std::make_unique<EditorBridge> (controller, [this] (const std::string& json) {
			if (webview)
			{
				const std::wstring w = widen (json);
				webview->PostWebMessageAsJson (w.c_str ());
			}
		});
		std::weak_ptr<Impl> weak = shared_from_this ();
		webview->add_WebMessageReceived (
		    Callback<ICoreWebView2WebMessageReceivedEventHandler> ([weak] (ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
			    auto self = weak.lock ();
			    if (!self || !self->bridge)
				    return S_OK;
			    LPWSTR raw = nullptr;
			    if (FAILED (args->TryGetWebMessageAsString (&raw)) || !raw)
				    return S_OK;
			    const std::string msg = narrow (raw);
			    CoTaskMemFree (raw);
			    if (msg.find ("\"drop\"") != std::string::npos)
				    self->handleDrop (args);
			    else
				    self->bridge->onWebMessage (msg);
			    return S_OK;
		    }).Get (),
		    &msgToken);

		wvController->put_IsVisible (TRUE);
		layout ();

		const std::string html = loadUiHtml ();
		if (html.empty ())
		{
			fail (L"Ressource d'interface introuvable dans le plugin.");
			return;
		}
		const std::wstring wh = widen (html);
		webview->NavigateToString (wh.c_str ());
		SetTimer (hwnd, kTimerId, 16, nullptr);
		if (hwnd)
			InvalidateRect (hwnd, nullptr, TRUE);
	}

	// Fichier déposé dans la gueule : la page nous transmet l'objet File, on en lit le chemin natif.
	void handleDrop (ICoreWebView2WebMessageReceivedEventArgs* args)
	{
		ComPtr<ICoreWebView2WebMessageReceivedEventArgs2> a2;
		if (FAILED (args->QueryInterface (IID_PPV_ARGS (&a2))) || !a2)
			return;
		ComPtr<ICoreWebView2ObjectCollectionView> objs;
		if (FAILED (a2->get_AdditionalObjects (&objs)) || !objs)
			return;
		UINT32 n = 0;
		objs->get_Count (&n);
		if (n == 0)
			return;
		ComPtr<IUnknown> o;
		if (FAILED (objs->GetValueAtIndex (0, &o)) || !o)
			return;
		ComPtr<ICoreWebView2File> file;
		if (FAILED (o.As (&file)) || !file)
			return;
		LPWSTR path = nullptr;
		if (SUCCEEDED (file->get_Path (&path)) && path)
		{
			bridge->onDroppedFile (narrow (path));
			CoTaskMemFree (path);
		}
	}

	void shutdown ()
	{
		if (hwnd)
			KillTimer (hwnd, kTimerId);
		bridge.reset (); // se désabonne du contrôleur
		if (webview)
			webview->remove_WebMessageReceived (msgToken);
		if (wvController)
			wvController->Close ();
		webview.Reset ();
		wvController.Reset ();
		if (hwnd)
		{
			SetWindowLongPtrW (hwnd, GWLP_USERDATA, 0);
			DestroyWindow (hwnd);
			hwnd = nullptr;
		}
		if (!className.empty ())
			UnregisterClassW (className.c_str (), static_cast<HINSTANCE> (moduleHandle));
		if (comInit)
			CoUninitialize ();
		comInit = false;
	}
};

LRESULT CALLBACK wndProc (HWND h, UINT m, WPARAM w, LPARAM l)
{
	if (m == WM_NCCREATE)
	{
		auto* cs = reinterpret_cast<CREATESTRUCTW*> (l);
		SetWindowLongPtrW (h, GWLP_USERDATA, reinterpret_cast<LONG_PTR> (cs->lpCreateParams));
		return DefWindowProcW (h, m, w, l);
	}
	auto* impl = reinterpret_cast<Impl*> (GetWindowLongPtrW (h, GWLP_USERDATA));
	if (impl)
	{
		switch (m)
		{
			case WM_TIMER:
				if (w == kTimerId && impl->bridge)
					impl->bridge->tick ();
				return 0;
			case WM_SIZE:
				impl->layout ();
				return 0;
			case WM_ERASEBKGND:
				if (impl->webview)
					return 1;
				break;
			case WM_PAINT:
			{
				PAINTSTRUCT ps;
				HDC dc = BeginPaint (h, &ps);
				RECT rc;
				GetClientRect (h, &rc);
				HBRUSH br = CreateSolidBrush (RGB (7, 6, 7));
				FillRect (dc, &rc, br);
				DeleteObject (br);
				if (!impl->webview)
				{
					SetBkMode (dc, TRANSPARENT);
					SetTextColor (dc, RGB (226, 52, 68));
					DrawTextW (dc, impl->status.c_str (), -1, &rc, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
				}
				EndPaint (h, &ps);
				return 0;
			}
			default: break;
		}
	}
	return DefWindowProcW (h, m, w, l);
}

class MimicEditorView : public Steinberg::CPluginView, public Steinberg::IPlugViewContentScaleSupport
{
public:
	explicit MimicEditorView (Controller* c)
	: CPluginView (nullptr), controller_ (c)
	{
		rect = Steinberg::ViewRect (0, 0, kEditorWidth, kEditorHeight);
	}

	Steinberg::tresult PLUGIN_API isPlatformTypeSupported (Steinberg::FIDString type) override
	{
		return std::strcmp (type, Steinberg::kPlatformTypeHWND) == 0 ? Steinberg::kResultTrue : Steinberg::kResultFalse;
	}

	Steinberg::tresult PLUGIN_API canResize () override { return Steinberg::kResultTrue; }

	Steinberg::tresult PLUGIN_API checkSizeConstraint (Steinberg::ViewRect* r) override
	{
		if (!r)
			return Steinberg::kResultFalse;
		const int w = std::max (static_cast<int> (kEditorWidth * 0.5f * scale_), r->getWidth ());
		r->right = r->left + w;
		r->bottom = r->top + (w * kEditorHeight + kEditorWidth / 2) / kEditorWidth; // aspect conservé
		return Steinberg::kResultTrue;
	}

	Steinberg::tresult PLUGIN_API onSize (Steinberg::ViewRect* r) override
	{
		if (r)
			rect = *r;
		if (impl_ && impl_->hwnd)
			SetWindowPos (impl_->hwnd, nullptr, 0, 0, rect.getWidth (), rect.getHeight (), SWP_NOZORDER | SWP_NOACTIVATE);
		return Steinberg::kResultTrue;
	}

	Steinberg::tresult PLUGIN_API setContentScaleFactor (ScaleFactor f) override
	{
		scale_ = f > 0.1f ? f : 1.0f;
		rect = Steinberg::ViewRect (0, 0, static_cast<int> (kEditorWidth * scale_ + 0.5f), static_cast<int> (kEditorHeight * scale_ + 0.5f));
		if (plugFrame && impl_ && impl_->hwnd)
			plugFrame->resizeView (this, &rect);
		return Steinberg::kResultTrue;
	}

	void attachedToParent () override
	{
		impl_ = std::make_shared<Impl> ();
		impl_->controller = controller_;
		const HRESULT co = CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED);
		impl_->comInit = SUCCEEDED (co); // RPC_E_CHANGED_MODE : l'hôte a déjà initialisé COM autrement, on n'y touche pas

		impl_->className = L"MimicEditor_" + std::to_wstring (reinterpret_cast<uintptr_t> (impl_.get ()));
		const auto hInst = static_cast<HINSTANCE> (moduleHandle);
		WNDCLASSEXW wc {};
		wc.cbSize = sizeof (wc);
		wc.lpfnWndProc = wndProc;
		wc.hInstance = hInst;
		wc.hCursor = LoadCursorW (nullptr, IDC_ARROW);
		wc.lpszClassName = impl_->className.c_str ();
		RegisterClassExW (&wc);
		impl_->hwnd = CreateWindowExW (0, impl_->className.c_str (), L"MIMIC", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0,
		                               rect.getWidth (), rect.getHeight (), static_cast<HWND> (systemWindow), nullptr, hInst, impl_.get ());
		if (impl_->hwnd)
			impl_->startWebView ();
	}

	void removedFromParent () override
	{
		if (impl_)
		{
			impl_->shutdown ();
			impl_.reset ();
		}
	}

	OBJ_METHODS (MimicEditorView, CPluginView)
	DEFINE_INTERFACES
		DEF_INTERFACE (Steinberg::IPlugViewContentScaleSupport)
	END_DEFINE_INTERFACES (CPluginView)
	REFCOUNT_METHODS (CPluginView)

private:
	Controller* controller_;
	std::shared_ptr<Impl> impl_;
	float scale_ = 1.0f;
};

} // namespace

Steinberg::IPlugView* createMimicEditor (Controller* c)
{
	return new MimicEditorView (c);
}

} // namespace mimic

#endif // _WIN32
