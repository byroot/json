require 'json'
# Warning[:deprecated] = true

test_data = {
  "flag" => true,
  "data" => 10000.times.map { [1.0] },
  :flag => false,
}

10.times do
  test_data.to_json
end
