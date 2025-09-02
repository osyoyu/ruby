# Copyright 2014 The Go Authors.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
#    * Redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer.
#    * Redistributions in binary form must reproduce the above
# copyright notice, this list of conditions and the following disclaimer
# in the documentation and/or other materials provided with the
# distribution.
#    * Neither the name of Google LLC nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.




# A Ruby script partially converted from the
# Profobuf encoder in Go's runtime/pprof.
# src/runtime/pprof/protobuf.go

module Ruby
  module Profiler
    class Protobuf
      attr_accessor :data, :nest

      def initialize
        @data = []
        @tmp = Array.new(16, 0)
        @nest = 0
      end

      def varint(x)
        while x >= 128
          @data << ((x & 0xFF) | 0x80)
          x >>= 7
        end
        @data << (x & 0xFF)
      end

      def length(tag, len)
        varint((tag << 3) | 2)
        varint(len)
      end

      def uint64(tag, x)
        # append varint to data
        varint((tag << 3) | 0)
        varint(x)
      end

      def uint64s(tag, x)
        if x.length > 2
          # Use packed encoding
          n1 = @data.length
          x.each { |u| varint(u) }
          n2 = @data.length
          length(tag, n2 - n1)
          n3 = @data.length
          @tmp[0...(n3 - n2)] = @data[n2...n3]
          @data[n1 + (n3 - n2)...n2 + (n3 - n2)] = @data[n1...n2]
          @data[n1...n1 + (n3 - n2)] = @tmp[0...(n3 - n2)]
          return
        end
        x.each { |u| uint64(tag, u) }
      end

      def uint64_opt(tag, x)
        return if x == 0
        uint64(tag, x)
      end

      def int64(tag, x)
        u = x
        uint64(tag, u)
      end

      def int64_opt(tag, x)
        return if x == 0
        int64(tag, x)
      end

      def int64s(tag, x)
        if x.length > 2
          # Use packed encoding
          n1 = @data.length
          x.each { |u| varint(u) }
          n2 = @data.length
          length(tag, n2 - n1)
          n3 = @data.length
          @tmp[0...(n3 - n2)] = @data[n2...n3]
          @data[n1 + (n3 - n2)...n2 + (n3 - n2)] = @data[n1...n2]
          @data[n1...n1 + (n3 - n2)] = @tmp[0...(n3 - n2)]
          return
        end
        x.each { |u| int64(tag, u) }
      end

      def string(tag, x)
        x = x.to_s  # Ensure x is a string
        length(tag, x.bytesize)
        @data.concat(x.bytes)
      end

      def strings(tag, x)
        x.each { |s| string(tag, s) }
      end

      def string_opt(tag, x)
        return if x == ""
        string(tag, x)
      end

      def bool(tag, x)
        uint64(tag, x ? 1 : 0)
      end

      def bool_opt(tag, x)
        return unless x
        bool(tag, x)
      end

      def start_message
        @nest += 1
        @data.length
      end

      def end_message(tag, start)
        n1 = start
        n2 = @data.length
        length(tag, n2 - n1)
        n3 = @data.length
        @tmp[0...(n3 - n2)] = @data[n2...n3]
        @data[n1 + (n3 - n2)...n2 + (n3 - n2)] = @data[n1...n2]
        @data[n1...n1 + (n3 - n2)] = @tmp[0...(n3 - n2)]
        @nest -= 1
      end
    end

    module SerializedProfile
      # Profile
      SAMPLE_TYPE         = 1
      SAMPLE              = 2
      MAPPING             = 3
      LOCATION            = 4
      FUNCTION            = 5
      STRING_TABLE        = 6
      DROP_FRAMES         = 7
      KEEP_FRAMES         = 8
      TIME_NANOS          = 9
      DURATION_NANOS      = 10
      PERIOD_TYPE         = 11
      PERIOD              = 12
      COMMENT             = 13
      DEFAULT_SAMPLE_TYPE = 14

      # ValueType
      VALUE_TYPE_TYPE = 1
      VALUE_TYPE_UNIT = 2

      # Sample
      SAMPLE_LOCATION = 1
      SAMPLE_VALUE    = 2
      SAMPLE_LABEL    = 3

      # Label
      LABEL_KEY = 1
      LABEL_STR = 2
      LABEL_NUM = 3

      # Mapping
      MAPPING_ID               = 1
      MAPPING_START            = 2
      MAPPING_LIMIT            = 3
      MAPPING_OFFSET           = 4
      MAPPING_FILENAME         = 5
      MAPPING_BUILD_ID         = 6
      MAPPING_HAS_FUNCTIONS    = 7
      MAPPING_HAS_FILENAMES    = 8
      MAPPING_HAS_LINE_NUMBERS = 9
      MAPPING_HAS_INLINE_FRAMES = 10

      # Location
      LOCATION_ID         = 1
      LOCATION_MAPPING_ID = 2
      LOCATION_ADDRESS    = 3
      LOCATION_LINE       = 4

      # Line
      LINE_FUNCTION_ID = 1
      LINE_LINE        = 2

      # Function
      FUNCTION_ID          = 1
      FUNCTION_NAME        = 2
      FUNCTION_SYSTEM_NAME = 3
      FUNCTION_FILENAME    = 4
      FUNCTION_START_LINE  = 5
    end

    module ProfileTags
      # Profile
      SAMPLE_TYPE         = 1
      SAMPLE              = 2
      MAPPING             = 3
      LOCATION            = 4
      FUNCTION            = 5
      STRING_TABLE        = 6
      DROP_FRAMES         = 7
      KEEP_FRAMES         = 8
      TIME_NANOS          = 9
      DURATION_NANOS      = 10
      PERIOD_TYPE         = 11
      PERIOD              = 12
      COMMENT             = 13
      DEFAULT_SAMPLE_TYPE = 14

      # ValueType
      VALUE_TYPE_TYPE = 1
      VALUE_TYPE_UNIT = 2

      # Sample
      SAMPLE_LOCATION = 1
      SAMPLE_VALUE    = 2
      SAMPLE_LABEL    = 3

      # Label
      LABEL_KEY = 1
      LABEL_STR = 2
      LABEL_NUM = 3

      # Mapping
      MAPPING_ID               = 1
      MAPPING_START            = 2
      MAPPING_LIMIT            = 3
      MAPPING_OFFSET           = 4
      MAPPING_FILENAME         = 5
      MAPPING_BUILD_ID         = 6
      MAPPING_HAS_FUNCTIONS    = 7
      MAPPING_HAS_FILENAMES    = 8
      MAPPING_HAS_LINE_NUMBERS = 9
      MAPPING_HAS_INLINE_FRAMES = 10

      # Location
      LOCATION_ID         = 1
      LOCATION_MAPPING_ID = 2
      LOCATION_ADDRESS    = 3
      LOCATION_LINE       = 4

      # Line
      LINE_FUNCTION_ID = 1
      LINE_LINE        = 2

      # Function
      FUNCTION_ID          = 1
      FUNCTION_NAME        = 2
      FUNCTION_SYSTEM_NAME = 3
      FUNCTION_FILENAME    = 4
      FUNCTION_START_LINE  = 5
    end

    class ProfileBuilder
      include ProfileTags

      attr_reader :pb, :strings, :string_map

      def initialize
        @start_time = Time.now
        @pb = Protobuf.new
        @strings = []
        @string_map = {}
        @locations = {}
        @functions = {}
        @mappings = []

        # Add empty string as first entry (index 0)
        string_index("")
      end

      # Add string to string table and return its index
      def string_index(str)
        return @string_map[str] if @string_map.key?(str)

        id = @strings.length
        @strings << str
        @string_map[str] = id
        id
      end

      # Encode a ValueType message
      def pb_value_type(tag, type, unit)
        start = @pb.start_message
        @pb.int64(VALUE_TYPE_TYPE, string_index(type))
        @pb.int64(VALUE_TYPE_UNIT, string_index(unit))
        @pb.end_message(tag, start)
      end

      # Encode a Sample message
      def pb_sample(values, location_ids, labels = nil)
        start = @pb.start_message
        @pb.int64s(SAMPLE_VALUE, values)
        @pb.uint64s(SAMPLE_LOCATION, location_ids)

        if labels
          labels.each do |key, value|
            pb_label(SAMPLE_LABEL, key, value.to_s, 0)
          end
        end

        @pb.end_message(SAMPLE, start)
      end

      # Encode a Label message
      def pb_label(tag, key, str, num)
        start = @pb.start_message
        @pb.int64_opt(LABEL_KEY, string_index(key))
        @pb.int64_opt(LABEL_STR, string_index(str))
        @pb.int64_opt(LABEL_NUM, num)
        @pb.end_message(tag, start)
      end

      # Encode a Location message
      def pb_location(id, mapping_id, address, lines)
        start = @pb.start_message
        @pb.uint64(LOCATION_ID, id)
        @pb.uint64_opt(LOCATION_MAPPING_ID, mapping_id)
        @pb.uint64(LOCATION_ADDRESS, address)

        lines.each do |func_id, line|
          pb_line(LOCATION_LINE, func_id, line)
        end

        @pb.end_message(LOCATION, start)
      end

      # Encode a Line message
      def pb_line(tag, func_id, line)
        start = @pb.start_message
        @pb.uint64_opt(LINE_FUNCTION_ID, func_id)
        @pb.int64_opt(LINE_LINE, line)
        @pb.end_message(tag, start)
      end

      # Encode a Function message
      def pb_function(id, name, system_name, filename, start_line)
        start = @pb.start_message
        @pb.uint64(FUNCTION_ID, id)
        @pb.int64(FUNCTION_NAME, string_index(name))
        @pb.int64_opt(FUNCTION_SYSTEM_NAME, string_index(system_name))
        @pb.int64_opt(FUNCTION_FILENAME, string_index(filename))
        @pb.int64_opt(FUNCTION_START_LINE, start_line)
        @pb.end_message(FUNCTION, start)
      end

      # Encode a Mapping message
      def pb_mapping(id, start_addr, limit, offset, filename, build_id)
        start = @pb.start_message
        @pb.uint64(MAPPING_ID, id)
        @pb.uint64(MAPPING_START, start_addr)
        @pb.uint64(MAPPING_LIMIT, limit)
        @pb.uint64_opt(MAPPING_OFFSET, offset)
        @pb.int64_opt(MAPPING_FILENAME, string_index(filename))
        @pb.int64_opt(MAPPING_BUILD_ID, string_index(build_id))
        @pb.bool_opt(MAPPING_HAS_FUNCTIONS, true)
        @pb.bool_opt(MAPPING_HAS_FILENAMES, true)
        @pb.bool_opt(MAPPING_HAS_LINE_NUMBERS, true)
        @pb.end_message(MAPPING, start)
      end

      # Build a CPU profile
      def build_cpu_profile(samples, period_ns = 10_000_000)
        end_time = Time.now

        # Profile metadata
        @pb.int64_opt(TIME_NANOS, @start_time.to_i * 1_000_000_000 + @start_time.nsec)
        @pb.int64_opt(DURATION_NANOS, ((end_time - @start_time) * 1_000_000_000).to_i)

        # Sample types
        pb_value_type(SAMPLE_TYPE, "samples", "count")
        pb_value_type(SAMPLE_TYPE, "cpu", "nanoseconds")

        # Period
        pb_value_type(PERIOD_TYPE, "cpu", "nanoseconds")
        @pb.int64(PERIOD, period_ns)

        # Add a fake mapping
        pb_mapping(1, 0x400000, 0x999999999, 0, "/bin/myprogram", "1234567890abcdef")

        # Add functions
        func_id = 1
        samples.each do |sample|
          sample[:stack].each do |frame|
            next if @functions[frame[:name]]

            pb_function(func_id, frame[:name], "", frame[:file] || "", frame[:line] || 0)
            @functions[frame[:name]] = func_id
            func_id += 1
          end
        end

        # Add locations
        loc_id = 1
        samples.each do |sample|
          sample[:stack].each do |frame|
            addr = frame[:address] || (0x400000 + loc_id * 0x100)
            if !@locations[addr]
              lines = [[(@functions[frame[:name]] || 0), frame[:line] || 0]]
              pb_location(loc_id, 1, addr, lines)
              @locations[addr] = loc_id
              loc_id += 1
            end
          end
        end

        # Add samples
        samples.each do |sample|
          location_ids = sample[:stack].map do |frame|
            addr = frame[:address] || (0x400000 + @locations.size * 0x100)
            @locations[addr] || 0
          end.reverse  # Stack traces are in reverse order

          values = [sample[:count], sample[:count] * period_ns]
          pb_sample(values, location_ids, sample[:labels])
        end

        # String table must be last
        @pb.strings(STRING_TABLE, @strings)

        # # Compress with gzip
        # buffer = StringIO.new
        # gz = Zlib::GzipWriter.new(buffer)
        # gz.write(@pb.data.pack("C*"))
        # gz.close

        # buffer.string
        @pb.data.pack("C*")
      end
    end

    class ProfileBuilderBuilder
      def initialize
        @samples = Hash.new(0)
      end

      def add_sample(stack)
        s = stack.reverse
        @samples[s] += 1
      end

      def to_profile
        builder_input = @samples.map do |stack, count|
          {
            count: count,
            stack: stack.map do |frame|
              {
                name: frame[:name],
                file: frame[:file],
                line: frame[:line],
                address: frame[:address]
              }
            end
          }
        end

        builder = ProfileBuilder.new
        builder.build_cpu_profile(builder_input)
      end
    end
  end
end
