/* xshot CLASS|title:NAME|0xID OUT.ppm: saves the window with WM_CLASS CLASS (or that
 * window ID) as it is on screen, for the README screenshots and UI tests. */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Window find(Display *d, Window w, const char *cls)
{
	XClassHint ch;
	if (!strncmp(cls, "title:", 6)) {
		char *name = NULL;
		XWindowAttributes a;
		if (XFetchName(d, w, &name) && name) {
			int match = !strcmp(name, cls + 6);
			XFree(name);
			if (match && XGetWindowAttributes(d, w, &a) && a.map_state == IsViewable)
				return w;
		}
	} else if (XGetClassHint(d, w, &ch)) {
		int match = ch.res_class && !strcmp(ch.res_class, cls);
		XFree(ch.res_name);
		XFree(ch.res_class);
		XWindowAttributes a;
		if (match && XGetWindowAttributes(d, w, &a) && a.map_state == IsViewable)
			return w;
	}
	Window root, parent, *kids;
	unsigned n;
	Window found = 0;
	if (XQueryTree(d, w, &root, &parent, &kids, &n)) {
		for (unsigned i = 0; i < n && !found; i++)
			found = find(d, kids[i], cls);
		XFree(kids);
	}
	return found;
}

int main(int argc, char **argv)
{
	if (argc != 3)
		return fprintf(stderr, "usage: xshot CLASS OUT.ppm\n"), 2;
	Display *d = XOpenDisplay(NULL);
	if (!d)
		return 1;
	Window w = !strncmp(argv[1], "0x", 2) ? (Window)strtoul(argv[1], NULL, 16)
					       : find(d, DefaultRootWindow(d), argv[1]);
	if (!w)
		return fprintf(stderr, "no window of class %s\n", argv[1]), 1;
	XWindowAttributes a;
	XGetWindowAttributes(d, w, &a);
	XImage *img = XGetImage(d, w, 0, 0, a.width, a.height, AllPlanes, ZPixmap);
	if (!img)
		return 1;
	FILE *f = fopen(argv[2], "wb");
	fprintf(f, "P6\n%d %d\n255\n", a.width, a.height);
	for (int y = 0; y < a.height; y++)
		for (int x = 0; x < a.width; x++) {
			unsigned long p = XGetPixel(img, x, y);
			unsigned char rgb[3] = {(p >> 16) & 255, (p >> 8) & 255, p & 255};
			fwrite(rgb, 1, 3, f);
		}
	fclose(f);
	return 0;
}
