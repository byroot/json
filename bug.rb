require 'json'
require 'active_support'
require 'active_support/core_ext/hash'

large_string = "x" * (35 * 1024 * 1024); nil
data = { "content" => large_string }.with_indifferent_access; nil

100.times do |i|
  json_data = JSON.dump(data)

  if json_data.getbyte(0) == 0
    puts "BUG: JSON.dump returned NULL bytes on attempt #{i}"
    puts "Size: #{json_data.bytesize} bytes"
    break
  end
end
