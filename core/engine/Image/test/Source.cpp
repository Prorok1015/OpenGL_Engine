#ifdef TESTLIB
#include <image.h>
#include <iostream>

int main()
{
	using namespace stb_image;
	Image img;
	img.flip(Image::ImageFlip::VERTICAL);
	img.open("test.jpg");
	img.write("write.png");
	system("pause");
}



#endif // TESTLIB

