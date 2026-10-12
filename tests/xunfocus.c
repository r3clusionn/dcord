/* xunfocus: moves keyboard focus to the root window (tests "window not focused"). */
#include <X11/Xlib.h>

int main(void)
{
	Display *d = XOpenDisplay(NULL);
	if (!d)
		return 1;
	XSetInputFocus(d, PointerRoot, RevertToPointerRoot, CurrentTime);
	XSync(d, False);
	XCloseDisplay(d);
	return 0;
}
