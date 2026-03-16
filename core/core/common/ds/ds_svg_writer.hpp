#pragma once 
#include <fstream>
#include <string>

namespace ds {

	class svg_writer {
	private:
		int width, height;
		std::ofstream file;

	public:
		svg_writer(const std::string& filename, int w, int h) : width(w), height(h) {
			file.open(filename);
			write_header();
		}

		~svg_writer() {
			if (file.is_open()) {
				write_footer();
				file.close();
			}
		}

		void write_header() {
			file << "<svg xmlns=\"http://www.w3.org/2000/svg\" "
				<< "width=\"" << width << "\" height=\"" << height << "\" "
				<< "viewBox=\"0 0 " << width << " " << height << "\">\n";
			file << "<g transform=\"scale(1,-1) translate(0,-" << height << ")\">\n";
			file << "<rect width=\"100%\" height=\"100%\" fill=\"white\"/>\n";
		}

		void write_footer() {
			file << "</g>\n";
			file << "</svg>";
		}

		void add_rect(const auto& r, std::string color = "red", uint32_t id = 0) {
			double px1 = r.min.x * width;
			double py1 = r.min.y * height;
			double px2 = r.max.x * width;
			double py2 = r.max.y * height;

			double x = std::min(px1, px2);
			double y = std::min(py1, py2);
			double w = std::abs(px1 - px2);
			double h = std::abs(py1 - py2);

			file << "<rect id=\"" << id << "\" x = \"" << x << "\" y=\"" << y << "\" "
				<< "width=\"" << w << "\" height=\"" << h << "\" "
				<< "fill=\"none\" stroke=\"" << color << "\" stroke-width=\"1\" "
				<< "opacity=\"0.7\" />\n";
		}
	};

}