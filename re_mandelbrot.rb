# Usage
# ./ruby re_mandelbrot.rb
# go tool pprof -http=localhost:3000 ./prof-2690777.pb.gz

# require 'pf2'
require 'chunky_png'
require 'json'

def mandelbrot_pixel(x, y, width, height, max_iter)
  real_part = (x - width / 2.0) * 4.0 / width
  imag_part = (y - height / 2.0) * 4.0 / height

  c = Complex(real_part, imag_part)
  z = 0
  iter = 0

  while iter < max_iter && z.magnitude <= 2
    z = z * z + c
    iter += 1
  end

  iter
end

def generate_mandelbrot_image(width, height, max_iter, num_threads)
  image = ChunkyPNG::Image.new(width, height, ChunkyPNG::Color::TRANSPARENT)
  threads = []
  num_threads.times do |thread_id|
    threads << Thread.new(thread_id) do |tid|
      sleep 1
      puts "New Thread: #{Thread.current.native_thread_id}"
      start_row = tid * (height / num_threads)
      end_row = (tid + 1) * (height / num_threads)

      (start_row...end_row).each do |y|
        width.times do |x|
          color_value = mandelbrot_pixel(x, y, width, height, max_iter)
          color = ChunkyPNG::Color.grayscale(color_value * 255 / max_iter)
          image[x, y] = color
        end
      end
      puts "done (#{thread_id})"
    end
  end
  threads.each(&:join)
  image
end

# Configure parameters
width = 800
height = 800
max_iter = 1000
threads = !ENV['THREADS'].nil? ? ENV['THREADS'].to_i : 16
p threads

# Generate mandelbrot set image
$start_time = Time.now
$ser2 = true

RubyVM::Profiler.enable
mandelbrot_image = generate_mandelbrot_image(width, height, max_iter, threads)
mandelbrot_image.save('mandelbrot.png')
puts "Mandelbrot image saved as 'mandelbrot.png'"
t = RubyVM::Profiler.disable
buffer = StringIO.new
gz = Zlib::GzipWriter.new(buffer)
gz.write(t)
gz.close
File.open("prof-#{Process.pid}.pb.gz", "wb") { |f| f.write(buffer.string) }

$end_time = Time.now
puts "Time taken: #{$end_time - $start_time} seconds"
